#include "hw_p4_dashboard.h"
#include "lvgl.h"
#include "lvgl_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_idf_version.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "src/display/lv_display_private.h"   /* inv_areas: области кадра для синхронизации буферов */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"
#include "lispif.h"
#include "flash_helper.h"
#include "esp_rom_crc.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "log.h"
#include "sdmmc_cmd.h"

static const char *TAG = "p4_dashboard";

/* Как ESP_ERROR_CHECK, но пишет в лог и возвращает управление вместо аварийного завершения, чтобы
 * остальная прошивка (USB, VESC Tool) продолжала работать, если панель не заработала. */
#define DASH_CHECK(expr) do {                                  \
        esp_err_t _e = (expr);                                 \
        if (_e != ESP_OK) {                                    \
            ESP_LOGE(TAG, "%s failed: %s", #expr, esp_err_to_name(_e)); \
            return;                                            \
        }                                                      \
    } while (0)

/* Дескрипторы оборудования: панель/шина DSI, panel IO, LDO и тачскрин GT911. s_flush_done отдаётся
 * из ISR «передача цвета DPI завершена», чтобы flush мог дождаться DMA.
 */
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_ldo_channel_handle_t s_ldo;
static SemaphoreHandle_t s_flush_done;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_touch_io;

/* Объекты LVGL. s_lvgl_mutex сериализует любое использование API LVGL между задачами. */
static lv_display_t *s_display;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_lvgl_mutex;
static TaskHandle_t s_lvgl_task;
/* true, если создать задачу LVGL не удалось: dashboard_lvgl_call() тогда возвращает false. */
static bool s_lvgl_task_failed = false;
/* true, пока запущенная flush_cb передача в панель ещё не подтверждена в flush_wait: без этого ожидание
 * без передачи в полёте простаивало бы весь таймаут. */
static volatile bool s_flush_inflight = false;
static TaskHandle_t s_lvgl_call_task = NULL;
/* ------------------------------------------------------------
 * Тройная буферизация кадра (DASH_TRIPLE_FB = 1)
 *
 * Панель (DPI) получает три полноэкранных буфера в PSRAM. Пока один показывается, LVGL рисует кадр в другой:
 * каждый отрисованный кусок поворачивается PPA прямо в этот буфер (без промежуточной копии). Готовый кадр
 * "предъявляется" одним вызовом: драйвер DPI переключает показ на него на границе кадра панели, поэтому
 * картинка никогда не рвётся посередине. Третий буфер позволяет начинать следующий кадр сразу, не дожидаясь
 * переключения.
 *
 * Новый кадр рисуется только в изменённых областях, поэтому перед этим в буфер копируется (тоже PPA) всё,
 * что изменилось за два кадра, которые он пропустил, -- кроме областей, которые нынешний кадр всё равно
 * перерисует целиком (при листании страниц копировать нечего).
 * ------------------------------------------------------------ */
#ifndef DASH_TRIPLE_FB
#define DASH_TRIPLE_FB 1
#endif
#define DASH_FB_DIRTY_MAX 16
typedef struct {
    lv_area_t a[DASH_FB_DIRTY_MAX];   /* в координатах панели */
    uint8_t n;
    bool full;                         /* слишком много областей: весь экран */
} dash_dirty_t;
static bool s_triple;                  /* режим включён (три буфера получены и PPA работает) */
static uint8_t *s_fb[3];
static size_t s_fb_size;
static dash_dirty_t s_fb_dirty[3];     /* что изменил кадр, нарисованный в этот буфер последним */
static uint32_t s_fb_pres_vs[3];       /* номер vsync в момент предъявления буфера */
static bool s_fb_presented[3] = { true, false, false };   /* fb0 показывается с самого старта */
static int s_fb_latest = 0;            /* последний предъявленный (показывается или вот-вот будет) */
static int s_fb_prev = -1;             /* предъявленный перед ним */
static int s_fb_draw = -1;             /* буфер рисуемого кадра, -1 = кадр не начат */
static volatile uint32_t s_vsync_cnt;
static SemaphoreHandle_t s_vsync_sem;
static bool s_vsync_dead;              /* кадры панели не сообщаются: ждать переключения бессмысленно */
static SemaphoreHandle_t s_ppa_done;   /* завершение неблокирующей операции PPA */
static volatile bool s_ppa_inflight;
static volatile uint32_t s_fb_sync_px; /* скопировано при синхронизации буферов (для lv-perf) */
static volatile bool s_bl_release_pending;   /* определена ниже, у подсветки */

/* Запросы от других задач (поток вычислений LispBM, usb_rx во время
 * lispif_restart()) выполняются внутри задачи LVGL: у неё большой
 * стек, и только ей разрешено трогать объекты LVGL.
 *
 * Вызовы идут строго по одному (s_lvgl_call_mutex), поэтому текущий вызов описывает одна запись s_call,
 * а очередь передаёт лишь "пробуждения". Состояние меняется под s_call_mux: пока задача LVGL не взяла
 * вызов (QUEUED), вызывающий может отменить его по таймауту и вернуть ошибку вместо вечного ожидания;
 * начатый вызов (RUNNING) всегда дожидается конца -- его аргументы лежат на стеке вызывающего. */
enum { LVGL_CALL_IDLE = 0, LVGL_CALL_QUEUED, LVGL_CALL_RUNNING, LVGL_CALL_DONE };
static struct {
    void (*fn)(void *arg);
    void *arg;
    volatile uint8_t state;
} s_call;
static portMUX_TYPE s_call_mux = portMUX_INITIALIZER_UNLOCKED;
/* Задача LVGL не взяла вызов за это время (зависла в отрисовке/ожидании панели): вызов отменяется. */
#define LVGL_CALL_START_MS   3000
/* Ожидание очереди вызывающих (другой вызов может законно идти долго, например сброс при перезапуске Lisp). */
#define LVGL_CALL_MUTEX_MS   15000
static volatile uint32_t s_lvgl_call_stalls;   /* сколько вызовов отменено из-за зависшей задачи LVGL */

typedef uint8_t dashboard_lvgl_req_t;   /* элемент очереди: только пробуждение */

static QueueHandle_t s_lvgl_req_queue;
static SemaphoreHandle_t s_lvgl_call_mutex;
static SemaphoreHandle_t s_lvgl_call_done;

/* Буферы отрисовки в байтах (RGB888, 3 байта/px). Рассчитаны на самую длинную логическую строку (800 px
 * в альбомной ориентации), чтобы частичная отрисовка работала при любом повороте. */
/* Строк в одном куске. Буферы лежат во ВНУТРЕННЕЙ RAM, если помещаются (отрисовка и программный
 * поворот в PSRAM измерены в ~160 нс/px), поэтому они небольшие; PSRAM — запасной вариант. */
#define LVGL_BUF_LINES 24
/* 0: выбрать самый большой кусок (8..24 строки), помещающийся во внутреннюю RAM (по умолчанию).
 * >0: принудительно столько строк на кусок, буферы в PSRAM (попробуйте 50). */
#ifndef DASH_PSRAM_LINES
#define DASH_PSRAM_LINES 120   /* кусок из 120 строк = 25% экрана в 480 строк; 0 = прежние малые куски во внутренней RAM */
#endif
/* внутренняя RAM, которая должна остаться свободной для Lisp / WiFi / BLE после выделения трёх буферов */
/* Размеры по умолчанию, когда DASH_PSRAM_LINES равен 0 (см. init). */
#define LVGL_INTERNAL_RESERVE (200 * 1024)
#define LVGL_BUF_MAX_BYTES (800 * LVGL_BUF_LINES * 3)
/* фактический размер куска в байтах, выбирается при init так, чтобы три буфера поместились во внутреннюю RAM (8..24 строки) */
static size_t s_buf_bytes = LVGL_BUF_MAX_BYTES;
#define LVGL_BUF_BYTES s_buf_bytes
static uint8_t *s_lv_buf1;
static uint8_t *s_lv_buf2;
/* Раскладка буферов: s_lv_buf1/2 — буферы частичной отрисовки LVGL (двойная буферизация, чтобы
 * отрисовка шла параллельно передаче по DMA); s_rot_buf хранит повёрнутую копию, отправляемую в панель.
 */
static uint8_t *s_rot_buf;   /* повёрнутая копия отрисованной области */
/* (lv-perf) дополнительно: время программного поворота, время ожидания DMA, число сброшенных пикселей */
/* Накопители 64-битные (на 32-битном RISC-V запись/чтение uint64_t не атомарна), поэтому все
 * обновления и чтение со сбросом выполняются внутри короткой критической секции s_perf_mux. */
static portMUX_TYPE s_perf_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_perf_rot_us = 0;
static uint64_t s_perf_wait_us = 0;
static uint64_t s_perf_px = 0;
static uint64_t s_perf_chunks = 0;
static uint64_t s_perf_flush_us = 0;

/* Перевод 64-битного накопителя в uint32_t с насыщением. */
static uint32_t dashboard_sat32(uint64_t v) {
    return v > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

/* Таблица инициализации скопирована из официального Waveshare BSP для
 * ESP32-P4-WIFI6-Touch-LCD-4.3 (esp32_p4_wifi6_touch_lcd_4_3.c). */
/* Записи имеют вид {cmd, data, data_len, delay_ms}. 0xFF выбирает командную страницу ST7701
 * (0x13/0x10/0x11 = расширенные страницы, 0x00 = возврат к обычной). 0x11 = sleep out (требуется
 * пауза 120 мс), 0x29 = включение дисплея.
 */
static const st7701_lcd_init_cmd_t vendor_specific_init_default[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x17, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09, 0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69}, 16, 0},
    {0xB1, (uint8_t[]){0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08, 0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x4E}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

/* Callback из ISR: DMA закончил копирование куска в панель; будит ожидающего flush.
 * IRAM_ATTR, потому что вызывается из прерывания DSI.
 */
static bool IRAM_ATTR dashboard_panel_color_done(
        esp_lcd_panel_handle_t panel,
        esp_lcd_dpi_panel_event_data_t *edata,
        void *user_ctx) {
    (void)panel;
    (void)edata;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

/* Callback из ISR: панель закончила очередной кадр (на старых ревизиях чипа -- DMA взял следующий кадр,
 * то есть только что применилось переключение буфера). Считаем кадры и будим ждущего свободный буфер. */
static bool IRAM_ATTR dashboard_panel_vsync(
        esp_lcd_panel_handle_t panel,
        esp_lcd_dpi_panel_event_data_t *edata,
        void *user_ctx) {
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_vsync_cnt++;
    BaseType_t hp_task_woken = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &hp_task_woken);
    }
    return hp_task_woken == pdTRUE;
}

/* ------------------------------------------------------------
 * Подсветка: ШИМ LEDC на LCD_BACKLIGHT_GPIO (5 кГц, 10 бит, как в
 * Waveshare BSP). На этой плате вывод активен по низкому уровню, поэтому выход LEDC
 * инвертируется, если LCD_BACKLIGHT_ON_LEVEL равен 0. Если LEDC настроить не удаётся,
 * вывод переходит на обычное включение/выключение через GPIO.
 * Используются таймер LEDC 3 / канал 7, чтобы Lisp-расширения pwm-*
 * (каналы 0..3) оставались свободными.
 * ------------------------------------------------------------ */
#define BL_LEDC_TIMER    LEDC_TIMER_3
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_7
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_BITS     LEDC_TIMER_10_BIT
#define BL_LEDC_MAX      ((1 << 10) - 1)
#define BL_LEDC_FREQ     5000

static bool s_bl_pwm_ok;
static volatile int s_bl_percent = 100;
/* Обычный mutex FreeRTOS (не спинлок): вызовы ledc_* могут блокироваться. Создаётся в
 * dashboard_backlight_init() (hw_init), защищает s_bl_percent/s_bl_hold/s_bl_armed и пару
 * ledc_set_duty/ledc_update_duty. */
static SemaphoreHandle_t s_bl_mutex;
static void dashboard_bl_lock(void) {
    if (s_bl_mutex) {
        xSemaphoreTake(s_bl_mutex, portMAX_DELAY);
    }
}
static void dashboard_bl_unlock(void) {
    if (s_bl_mutex) {
        xSemaphoreGive(s_bl_mutex);
    }
}

/* Включение питания: подсветка остаётся тёмной до первого полного кадра,
 * нарисованного после того, как скрипт запросил яркость (lv-backlight), чтобы
 * панель никогда не показывала пустой, наполовину нарисованный или «ждущий» экран. Запасной вариант:
 * подсветка всё равно включается через BL_HOLD_MAX_MS (нет скрипта или ошибка скрипта). */
#define BL_HOLD_MAX_MS   4000
static volatile bool s_bl_hold = true;
static volatile bool s_bl_armed = false;
static void dashboard_backlight_apply(int percent);

/* Запасной вариант: простое включение/выключение (без ШИМ). */
static void dashboard_backlight_gpio(bool on) {
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);
    gpio_set_level(LCD_BACKLIGHT_GPIO, on ? LCD_BACKLIGHT_ON_LEVEL : !LCD_BACKLIGHT_ON_LEVEL);
}

/* Настройка LEDC для ШИМ-регулировки; старт со скважностью 0, чтобы панель оставалась тёмной при загрузке. */
static void dashboard_backlight_init(void) {
    if (!s_bl_mutex) {
        s_bl_mutex = xSemaphoreCreateMutex();
    }
    ledc_timer_config_t tcfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t ccfg = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,               /* тёмная до первого кадра, см. s_bl_hold */
        .hpoint = 0,
        .flags.output_invert = (LCD_BACKLIGHT_ON_LEVEL == 0) ? 1 : 0,
    };

    s_bl_pwm_ok = ledc_timer_config(&tcfg) == ESP_OK &&
            ledc_channel_config(&ccfg) == ESP_OK;

    if (!s_bl_pwm_ok) {
        ESP_LOGW(TAG, "Backlight PWM unavailable, using on/off GPIO");
        dashboard_backlight_gpio(false);
    }
    s_bl_percent = 100;
}

/* 0 = выкл., 100 = полная яркость. Можно вызывать из любой задачи. Пока действует удержание
 * при включении питания, значение только сохраняется. */
void dashboard_backlight_percent(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    dashboard_bl_lock();
    s_bl_percent = percent;
    if (!s_bl_hold) {
        dashboard_backlight_apply(percent);
    }
    dashboard_bl_unlock();
}

/* Вызывается из (lv-backlight): во время удержания при включении питания подсветка
 * включается после следующего полного кадра. */
void dashboard_backlight_request(int percent) {
    dashboard_backlight_percent(percent);
    dashboard_bl_lock();
    if (s_bl_hold) {
        s_bl_armed = true;
    }
    dashboard_bl_unlock();
}

/* Завершить удержание при включении питания и применить сохранённую яркость (первый кадр или таймаут). */
static void dashboard_backlight_release(void) {
    dashboard_bl_lock();
    if (s_bl_hold) {
        s_bl_hold = false;
        s_bl_armed = false;
        dashboard_backlight_apply(s_bl_percent);
    }
    dashboard_bl_unlock();
}

/* Таймер LVGL: страховка, чтобы экран загорелся, даже если ни один скрипт этого не запросил. */
static void dashboard_bl_hold_timeout(lv_timer_t *t) {
    (void)t;
    dashboard_backlight_release();
}

/* Записать скважность (или уровень GPIO) для заданного процента. */
static void dashboard_backlight_apply(int percent) {
    if (!s_bl_pwm_ok) {
        dashboard_backlight_gpio(percent > 0);
        return;
    }

    /* Грубая гамма, чтобы ползунок ощущался равномерным: duty = p^2 / 100^2. */
    uint32_t duty = (uint32_t)percent * (uint32_t)percent * BL_LEDC_MAX / 10000U;
    if (percent > 0 && duty == 0) {
        duty = 1;
    }
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

int dashboard_backlight_get(void) {
    return s_bl_percent;
}

/* ------------------------------------------------------------
 * SD-карта
 *
 * Слот SD (GPIO 39..44) находится в домене IO, который питается от
 * встроенного канала LDO 4 (3,3 В), как в Waveshare BSP. При
 * HW_EARLY_LBM_INIT функция hw_init() выполняется до того, как main.c монтирует карту, поэтому
 * здесь включается только LDO; main.c монтирует карту позже
 * (log_mount_card), когда дашборд уже работает. Отсутствие монтирования
 * здесь не даёт отсутствующей карте задержать первый экран.
 * ------------------------------------------------------------ */
static esp_ldo_channel_handle_t s_ldo_sd;

static void dashboard_sd_init(void) {
#ifdef SD_PIN_MOSI
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_sd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD: LDO4 not available (%s)", esp_err_to_name(err));
    }
#endif
}

/* Запуск панели: LDO3 (2,5 В, питание MIPI DSI PHY), шина DSI, командный IO DBI,
 * затем режим видео DPI с последовательностью инициализации ST7701. При любой ошибке s_panel остаётся NULL.
 */
static void dashboard_display_init(void) {
#if !SOC_MIPI_DSI_SUPPORTED
    ESP_LOGE(TAG, "MIPI DSI is not supported by this ESP32-P4 target");
    return;
#else
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    DASH_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo));

    esp_lcd_dsi_bus_config_t bus_cfg = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    bus_cfg.lane_bit_rate_mbps = LCD_LANE_MBPS;
    DASH_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = ST7701_PANEL_IO_DBI_CONFIG();
    DASH_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_panel_io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLOCK_MHZ,
        .virtual_channel = 0,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
#else
        .in_color_format = LCD_COLOR_FMT_RGB888,
#endif
        .num_fbs = DASH_TRIPLE_FB ? 3 : 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_back_porch = 42,
            .hsync_pulse_width = 12,
            .hsync_front_porch = 42,
            /* Тайминги из Waveshare BSP для этой платы. */
            .vsync_back_porch = 2,
            .vsync_pulse_width = 8,
            .vsync_front_porch = 60,
        },
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .flags.use_dma2d = true,
#endif
    };

    st7701_vendor_config_t vendor_cfg = {
        .init_cmds = vendor_specific_init_default,
        .init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(vendor_specific_init_default[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    DASH_CHECK(esp_lcd_new_panel_st7701(s_panel_io, &panel_cfg, &panel));
    DASH_CHECK(esp_lcd_panel_reset(panel));
    DASH_CHECK(esp_lcd_panel_init(panel));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    DASH_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel));
#endif
    DASH_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    s_flush_done = xSemaphoreCreateBinary();
    s_vsync_sem = xSemaphoreCreateBinary();
    if (s_flush_done) {
        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_color_trans_done = dashboard_panel_color_done,
            .on_refresh_done = dashboard_panel_vsync,
        };
        DASH_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, s_flush_done));
    }
#if DASH_TRIPLE_FB
    {
        void *f0 = NULL, *f1 = NULL, *f2 = NULL;
        if (esp_lcd_dpi_panel_get_frame_buffer(panel, 3, &f0, &f1, &f2) == ESP_OK && f0 && f1 && f2) {
            s_fb[0] = f0;
            s_fb[1] = f1;
            s_fb[2] = f2;
            s_fb_size = (size_t)LCD_H_RES * LCD_V_RES * 3;
        } else {
            ESP_LOGW(TAG, "triple frame buffers are not available, single buffer is used");
        }
    }
#endif

    /* Панель пригодна к использованию только после успеха всех шагов выше. */
    s_panel = panel;

    ESP_LOGI(TAG, "ST7701 %ux%u initialized", LCD_H_RES, LCD_V_RES);
#endif
}

/* Запуск I2C и контроллера касаний GT911. Возвращает false, если он не найден;
 * дисплей работает и без касаний.
 */
static bool dashboard_touch_init(void) {
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(TOUCH_I2C_PORT, &i2c_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(err));
        return false;
    }

    /* На плате вывод INT GT911 не подключён к P4. Используется сброс, данные опрашиваются. */
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_set_level(TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    /* scl_speed_hz равен 0, потому что шина I2C уже настроена выше (legacy-драйвер). */
    io_cfg.scl_speed_hz = 0;
    io_cfg.dev_addr = TOUCH_ADDR_PRIMARY;

    err = esp_lcd_new_panel_io_i2c(TOUCH_I2C_PORT, &io_cfg, &s_touch_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 I2C IO failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_touch_io_gt911_config_t gt911_cfg = {
        .dev_addr = io_cfg.dev_addr,
    };

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = &gt911_cfg,
    };

    err = esp_lcd_touch_new_i2c_gt911(s_touch_io, &touch_cfg, &s_touch);
    if (err != ESP_OK) {
        /* Адрес GT911 (0x5D или 0x14) зависит от уровней INT/RST при сбросе, поэтому проверяем оба. */
        /* Пробуем альтернативный адрес GT911, задаваемый strap-ом контроллера. */
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
        io_cfg.dev_addr = TOUCH_ADDR_SECONDARY;
        gt911_cfg.dev_addr = io_cfg.dev_addr;
        err = esp_lcd_new_panel_io_i2c(TOUCH_I2C_PORT, &io_cfg, &s_touch_io);
        if (err == ESP_OK) {
            err = esp_lcd_touch_new_i2c_gt911(s_touch_io, &touch_cfg, &s_touch);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "GT911 initialized at 0x%02X", io_cfg.dev_addr);
    return true;
}

/* Аппаратный поворот: PPA (pixel processing accelerator) поворачивает отрисованный кусок на 90/270 градусов через DMA,
 * CPU только ждёт. LVGL ROTATION_90 поворачивает против часовой стрелки (rotate90_rgb888: исходный верхний правый угол ->
 * верхний левый угол результата), угол PPA тоже против часовой стрелки. Если картинка получается перевёрнутой,
 * поменяйте здесь два угла местами. При любой ошибке используется программный поворот. */
#define DASH_PPA_ANGLE_LV90  PPA_SRM_ROTATION_ANGLE_90
#define DASH_PPA_ANGLE_LV270 PPA_SRM_ROTATION_ANGLE_270
static ppa_client_handle_t s_ppa_srm;
static bool s_ppa_ok;
static bool s_bufs_internal;
bool dashboard_lvgl_bufs_internal(void) { return s_bufs_internal; }
bool dashboard_lvgl_ppa_on(void) { return s_ppa_ok; }
uint32_t dashboard_lvgl_psram_free(void) { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
uint32_t dashboard_lvgl_internal_free(void) { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static volatile uint32_t s_ppa_fail;
static volatile int32_t s_ppa_last = 1000; /* 1000 = ещё не пробовали, -2 = stride/формат не подходит, 0 ок, иначе esp_err */
static volatile uint32_t s_ppa_calls;
int32_t dashboard_lvgl_ppa_status(void) { return s_ppa_ok ? s_ppa_last : -1; }

/* Повернуть кусок RGB888 размером w*h в dst (h*w) с помощью PPA. Блокирующий вызов, но CPU простаивает
 * во время DMA. Возвращает false при ошибке, чтобы вызывающий мог использовать программный поворот.
 */
static bool dashboard_ppa_rotate(const uint8_t *src, uint8_t *dst, int32_t w, int32_t h, lv_display_rotation_t rot) {
    if (!s_ppa_ok) {
        return false;
    }
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = src,
            .pic_w = (uint32_t)w,
            .pic_h = (uint32_t)h,
            .block_w = (uint32_t)w,
            .block_h = (uint32_t)h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = dst,
            /* ёмкость выходного буфера */
            .buffer_size = LVGL_BUF_BYTES,
            .pic_w = (uint32_t)h,
            .pic_h = (uint32_t)w,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = (rot == LV_DISPLAY_ROTATION_90) ? DASH_PPA_ANGLE_LV90 : DASH_PPA_ANGLE_LV270,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = NULL,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg);
    s_ppa_last = (int32_t)err;
    s_ppa_calls++;
    if (err != ESP_OK) {
        if (s_ppa_fail++ == 0) {
            ESP_LOGW(TAG, "PPA rotate failed (%s), software rotation is used", esp_err_to_name(err));
        }
        return false;
    }
    return true;
}

/* Завершение неблокирующей операции PPA (ISR). */
static bool IRAM_ATTR dashboard_ppa_done_cb(ppa_client_handle_t client, ppa_event_data_t *ev, void *user_data) {
    (void)client;
    (void)ev;
    (void)user_data;
    BaseType_t w = pdFALSE;
    if (s_ppa_done) {
        xSemaphoreGiveFromISR(s_ppa_done, &w);
    }
    return w == pdTRUE;
}

static void dashboard_ppa_wait(void) {
    if (s_ppa_inflight) {
        if (xSemaphoreTake(s_ppa_done, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGE(TAG, "PPA did not finish in 200 ms");
        }
        s_ppa_inflight = false;
    }
}

/* Кусок w*h (логические координаты) повернуть PPA прямо в буфер кадра fb, в область панели dst.
 * blocking=false: вернуться сразу, завершение ждёт dashboard_ppa_wait(). */
static bool dashboard_ppa_to_fb(const uint8_t *src, uint8_t *fb, int32_t w, int32_t h, const lv_area_t *dst,
        lv_display_rotation_t rot, bool blocking) {
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = src,
            .pic_w = (uint32_t)w,
            .pic_h = (uint32_t)h,
            .block_w = (uint32_t)w,
            .block_h = (uint32_t)h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = fb,
            .buffer_size = s_fb_size,
            .pic_w = LCD_H_RES,
            .pic_h = LCD_V_RES,
            .block_offset_x = (uint32_t)dst->x1,
            .block_offset_y = (uint32_t)dst->y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = (rot == LV_DISPLAY_ROTATION_90) ? DASH_PPA_ANGLE_LV90 : DASH_PPA_ANGLE_LV270,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = blocking ? PPA_TRANS_MODE_BLOCKING : PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = NULL,
    };
    if (!blocking) {
        xSemaphoreTake(s_ppa_done, 0);   /* сброс отдачи от предыдущей (блокирующей) операции */
    }
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg);
    s_ppa_last = (int32_t)err;
    s_ppa_calls++;
    if (err != ESP_OK) {
        if (s_ppa_fail++ == 0) {
            ESP_LOGW(TAG, "PPA rotate to frame buffer failed (%s), software rotation is used", esp_err_to_name(err));
        }
        return false;
    }
    if (!blocking) {
        s_ppa_inflight = true;
    }
    return true;
}

/* Скопировать прямоугольник (координаты панели) из буфера src в буфер dst (PPA без поворота, блокирующе). */
static void dashboard_fb_copy(const uint8_t *src, uint8_t *dst, const lv_area_t *a) {
    int32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
    if (w <= 0 || h <= 0) {
        return;
    }
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = src,
            .pic_w = LCD_H_RES,
            .pic_h = LCD_V_RES,
            .block_w = (uint32_t)w,
            .block_h = (uint32_t)h,
            .block_offset_x = (uint32_t)a->x1,
            .block_offset_y = (uint32_t)a->y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = dst,
            .buffer_size = s_fb_size,
            .pic_w = LCD_H_RES,
            .pic_h = LCD_V_RES,
            .block_offset_x = (uint32_t)a->x1,
            .block_offset_y = (uint32_t)a->y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = NULL,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg) != ESP_OK) {
        /* запасной путь: копирование процессором и сброс кэша */
        size_t row = (size_t)LCD_H_RES * 3;
        for (int32_t y = a->y1; y <= a->y2; y++) {
            memcpy(dst + y * row + a->x1 * 3, src + y * row + a->x1 * 3, (size_t)w * 3);
        }
        esp_cache_msync(dst + a->y1 * row, (size_t)h * row, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    s_fb_sync_px += (uint32_t)(w * h);
}

static bool dash_area_in(const lv_area_t *in, const lv_area_t *out) {
    return in->x1 >= out->x1 && in->y1 >= out->y1 && in->x2 <= out->x2 && in->y2 <= out->y2;
}

/* Начало кадра: выбрать свободный буфер, запомнить области кадра и досинхронизировать пропущенное. */
static void dashboard_frame_begin(lv_display_t *display) {
    int t = (s_fb_prev < 0) ? (s_fb_latest + 1) % 3 : 3 - s_fb_latest - s_fb_prev;
    /* t освобождается, когда его сменил s_fb_prev и это переключение точно произошло (2 кадра панели) */
    if (s_fb_prev >= 0 && s_fb_presented[t] && !s_vsync_dead) {
        int64_t w0 = esp_timer_get_time();
        uint32_t v0 = s_vsync_cnt;
        for (int i = 0; i < 6 && (uint32_t)(s_vsync_cnt - s_fb_pres_vs[s_fb_prev]) < 2; i++) {
            xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(20));
        }
        if (s_vsync_cnt == v0 && (uint32_t)(s_vsync_cnt - s_fb_pres_vs[s_fb_prev]) < 2) {
            /* за 120 мс ни одного кадра панели: событие не приходит, дальше не ждём (возможны разрывы) */
            s_vsync_dead = true;
            ESP_LOGE(TAG, "no frame-done events from the panel, frame pacing is off");
        }
        uint32_t wdt = (uint32_t)(esp_timer_get_time() - w0);
        portENTER_CRITICAL(&s_perf_mux);
        s_perf_wait_us += wdt;
        portEXIT_CRITICAL(&s_perf_mux);
    }

    /* области этого кадра (LVGL уже объединил их) в координатах панели */
    dash_dirty_t cur = { .n = 0, .full = false };
    for (uint32_t i = 0; i < display->inv_p; i++) {
        if (display->inv_area_joined[i]) {
            continue;
        }
        lv_area_t a = display->inv_areas[i];
        lv_display_rotate_area(display, &a);
        if (cur.n < DASH_FB_DIRTY_MAX) {
            cur.a[cur.n++] = a;
        } else {
            cur.full = true;
        }
    }

    /* пропущенные буфером t кадры: s_fb_prev и s_fb_latest (если t их не рисовал) */
    const lv_area_t full = { 0, 0, LCD_H_RES - 1, LCD_V_RES - 1 };
    int src = s_fb_latest;
    int miss[2] = { s_fb_prev, s_fb_latest };
    for (int m = 0; m < 2; m++) {
        int f = miss[m];
        if (f < 0 || f == t || !s_fb_presented[f]) {
            continue;
        }
        const dash_dirty_t *d = &s_fb_dirty[f];
        int n = d->full ? 1 : d->n;
        for (int k = 0; k < n; k++) {
            const lv_area_t *r = d->full ? &full : &d->a[k];
            bool covered = cur.full;
            for (int c = 0; !covered && c < cur.n; c++) {
                covered = dash_area_in(r, &cur.a[c]);
            }
            if (!covered) {
                dashboard_fb_copy(s_fb[src], s_fb[t], r);
            }
        }
    }
    s_fb_dirty[t] = cur;
    s_fb_draw = t;
}

/* Конец кадра: всё нарисовано в s_fb[s_fb_draw] -- предъявить его панели (переключение на границе кадра). */
static void dashboard_frame_present(void) {
    int t = s_fb_draw;
    if (t < 0) {
        return;
    }
    dashboard_ppa_wait();
    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, 0);
    }
    /* указатель внутри буфера кадра: драйвер не копирует, а только переключает показ */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 1, 1, s_fb[t]);
    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, 0);   /* draw_bitmap без копирования сразу вызывает color_done */
    }
    s_fb_pres_vs[t] = s_vsync_cnt;
    s_fb_presented[t] = true;
    s_fb_prev = s_fb_latest;
    s_fb_latest = t;
    s_fb_draw = -1;
}

/* Flush-callback в режиме тройной буферизации. */
static void dashboard_lvgl_flush_triple(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    lv_display_rotation_t rot = lv_display_get_rotation(display);
    int64_t fl_t0 = esp_timer_get_time();
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    portENTER_CRITICAL(&s_perf_mux);
    s_perf_chunks++;
    s_perf_px += (uint32_t)(w * h);
    portEXIT_CRITICAL(&s_perf_mux);

    if (s_fb_draw < 0) {
        dashboard_ppa_wait();
        dashboard_frame_begin(display);
    }
    uint8_t *fb = s_fb[s_fb_draw];
    lv_area_t a = *area;
    lv_display_rotate_area(display, &a);
    bool last = lv_display_flush_is_last(display);

    int64_t rot_t0 = esp_timer_get_time();
    dashboard_ppa_wait();   /* PPA одна: предыдущий кусок должен закончиться */
    bool hw = (rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270) &&
            dashboard_ppa_to_fb(px_map, fb, w, h, &a, rot, last);
    if (!hw) {
        /* программный поворот прямо в буфер кадра + сброс кэша этих строк */
        lv_color_format_t cf = lv_display_get_color_format(display);
        uint32_t src_stride = lv_draw_buf_width_to_stride(w, cf);
        uint32_t row = (uint32_t)LCD_H_RES * 3;
        uint8_t *dst = fb + (size_t)a.y1 * row + (size_t)a.x1 * 3;
        if (rot == LV_DISPLAY_ROTATION_0) {
            for (int32_t y = 0; y < h; y++) {
                memcpy(dst + (size_t)y * row, px_map + (size_t)y * src_stride, (size_t)w * 3);
            }
        } else {
            lv_draw_sw_rotate(px_map, dst, w, h, src_stride, row, rot, cf);
        }
        esp_cache_msync(fb + (size_t)a.y1 * row, (size_t)lv_area_get_height(&a) * row,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    uint32_t rot_dt = (uint32_t)(esp_timer_get_time() - rot_t0);

    if (last) {
        dashboard_frame_present();
        if (s_bl_armed) {
            s_bl_release_pending = true;
        }
    }
    uint32_t fl_dt = (uint32_t)(esp_timer_get_time() - fl_t0);
    portENTER_CRITICAL(&s_perf_mux);
    s_perf_rot_us += rot_dt;
    s_perf_flush_us += fl_dt;
    portEXIT_CRITICAL(&s_perf_mux);
}

/* Flush-callback LVGL. Панель имеет родную портретную ориентацию, LVGL рисует в повёрнутой (логической)
 * системе координат, поэтому каждый отрисованный кусок поворачивается (аппаратно через PPA или программно)
 * в s_rot_buf и затем отправляется в панель по DMA. Счётчики времени питают (lv-perf).
 */
/* Отложенное включение подсветки: взводится в flush-callback на последнем куске первого кадра,
 * выполняется после завершения DMA (в flush_wait) либо, как запасной путь, в задаче LVGL
 * после короткой паузы (если LVGL не вызвал wait после последнего куска). */
static volatile bool s_bl_release_pending = false;
static void dashboard_bl_release_if_pending(void) {
    if (s_bl_release_pending) {
        s_bl_release_pending = false;
        dashboard_backlight_release();
    }
}

static void dashboard_lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    if (s_triple) {
        dashboard_lvgl_flush_triple(display, area, px_map);
        return;
    }
    lv_display_rotation_t rot = lv_display_get_rotation(display);
    lv_area_t a = *area;
    const uint8_t *pix = px_map;
    int64_t fl_t0 = esp_timer_get_time();
    uint32_t chunk_px = (uint32_t)(lv_area_get_width(area) * lv_area_get_height(area));
    portENTER_CRITICAL(&s_perf_mux);
    s_perf_chunks++;
    s_perf_px += chunk_px;
    portEXIT_CRITICAL(&s_perf_mux);

    if (rot != LV_DISPLAY_ROTATION_0 && s_rot_buf) {
        /* Программный поворот: LVGL рисует в логических (альбомных) координатах,
         * панель имеет родную портретную ориентацию 480x800. */
        lv_color_format_t cf = lv_display_get_color_format(display);
        int32_t w = lv_area_get_width(area);
        int32_t h = lv_area_get_height(area);
        lv_display_rotate_area(display, &a);
        uint32_t src_stride = lv_draw_buf_width_to_stride(w, cf);
        uint32_t dst_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&a), cf);
        int64_t rot_t0 = esp_timer_get_time();
        /* PPA требует плотно упакованный источник (stride = width * 3) и поворот на 90/270 */
        bool hw = (rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270) &&
                cf == LV_COLOR_FORMAT_RGB888 && src_stride == (uint32_t)(w * 3) &&
                dashboard_ppa_rotate(px_map, s_rot_buf, w, h, rot);
        if (!hw && !(cf == LV_COLOR_FORMAT_RGB888 && src_stride == (uint32_t)(w * 3))) {
            s_ppa_last = -2000 - (int32_t)cf * 10 - (int32_t)(src_stride - (uint32_t)(w * 3) > 99 ? 9 : src_stride - (uint32_t)(w * 3));
        }
        if (!hw) {
            lv_draw_sw_rotate(px_map, s_rot_buf, w, h, src_stride, dst_stride, rot, cf);
        }
        uint32_t rot_dt = (uint32_t)(esp_timer_get_time() - rot_t0);
        portENTER_CRITICAL(&s_perf_mux);
        s_perf_rot_us += rot_dt;
        portEXIT_CRITICAL(&s_perf_mux);
        pix = s_rot_buf;
    }

    /* Асинхронно: запускаем DMA-копирование и возвращаемся, LVGL тем временем рисует следующий
     * кусок и ждёт в dashboard_lvgl_flush_wait() перед очередным
     * flush (только после этого буфер поворота используется повторно). */
    bool ok = hw_p4_dashboard_flush_start(a.x1, a.y1, a.x2, a.y2, pix);
    if (!ok) {
        ESP_LOGE(TAG, "LVGL flush failed");
    } else {
        s_flush_inflight = true;
    }
    /* Включение питания: первый полный кадр после (lv-backlight) -> зажигаем подсветку.
     * Подсветка включается не здесь (DMA последнего куска ещё идёт), а только после ожидания
     * завершения передачи: см. dashboard_bl_release_if_pending(). */
    bool last = lv_display_flush_is_last(display);
    if (s_bl_armed && last) {
        s_bl_release_pending = true;
    }
    uint32_t fl_dt = (uint32_t)(esp_timer_get_time() - fl_t0);
    portENTER_CRITICAL(&s_perf_mux);
    s_perf_flush_us += fl_dt;
    portEXIT_CRITICAL(&s_perf_mux);
}

/* Вызывается LVGL перед повторным использованием буферов: ждёт завершения предыдущего DMA.
 * Ждём шагами по 200 мс, всего до 1000 мс: при таймауте нельзя продолжать сразу, иначе следующий
 * flush перезапишет s_rot_buf/буфер отрисовки, пока DMA ещё читает его. */
static void dashboard_lvgl_flush_wait(lv_display_t *display) {
    (void)display;
    if (s_triple) {
        int64_t wt0 = esp_timer_get_time();
        dashboard_ppa_wait();
        uint32_t wait_dt = (uint32_t)(esp_timer_get_time() - wt0);
        portENTER_CRITICAL(&s_perf_mux);
        s_perf_wait_us += wait_dt;
        portEXIT_CRITICAL(&s_perf_mux);
        return;
    }
    if (s_flush_done && s_flush_inflight) {
        static bool warned = false;
        int64_t wt0 = esp_timer_get_time();
        bool got = false;
        for (int i = 0; i < 5 && !got; i++) {
            got = xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200)) == pdTRUE;
            if (!got && !warned) {
                warned = true;
                ESP_LOGW(TAG, "flush wait: DMA not done in 200 ms, still waiting");
            }
        }
        s_flush_inflight = false;
        if (!got) {
            ESP_LOGE(TAG, "flush wait: DMA did not finish in 1000 ms, giving up");
            /* Поздняя отдача из ISR брошенной передачи не должна ускорить следующее ожидание. */
            xSemaphoreTake(s_flush_done, 0);
        }
        uint32_t wait_dt = (uint32_t)(esp_timer_get_time() - wt0);
        portENTER_CRITICAL(&s_perf_mux);
        s_perf_wait_us += wait_dt;
        portEXIT_CRITICAL(&s_perf_mux);
        if (got) {
            /* Первый кадр полностью ушёл в панель: можно зажигать подсветку. */
            dashboard_bl_release_if_pending();
        }
    }
}

/* Callback чтения указателя LVGL: опрашивает GT911 (линии прерывания нет) и сообщает об одной точке касания. */
static void dashboard_lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (!s_touch) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* Последнее удачное состояние: при единичных сбоях чтения I2C не рвём жест ложным отпусканием. */
    static lv_point_t s_last_pt;
    static lv_indev_state_t s_last_state = LV_INDEV_STATE_RELEASED;
    static uint8_t s_touch_err = 0;

    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        if (s_touch_err < 3) {
            s_touch_err++;
            data->point = s_last_pt;
            data->state = s_last_state;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
            s_last_state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }
    s_touch_err = 0;

    esp_lcd_touch_point_data_t points[1];
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch, points, &count, 1) == ESP_OK && count > 0) {
        /* Сырые координаты панели (родная портретная ориентация 480x800). LVGL поворачивает
         * ввод указателя вместе с поворотом дисплея (lv_display_rotate_point),
         * поэтому касания всегда автоматически следуют LCD_ROTATION. */
        data->point.x = points[0].x;
        data->point.y = points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
        s_last_pt = data->point;
        s_last_state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_last_state = LV_INDEV_STATE_RELEASED;
    }
}

/* Захват/освобождение mutex LVGL. Любая задача, кроме задачи LVGL, должна удерживать его при
 * вызове функций LVGL.
 */
/* Счётчик вложенных захватов: меняется только задачей-владельцем mutex, поэтому отдельной
 * защиты не требует. Повторный lock из задачи, уже владеющей mutex, только увеличивает его. */
static volatile uint32_t s_lvgl_lock_nest = 0;

void dashboard_lvgl_lock(void) {
    if (s_lvgl_mutex) {
        /* Уже владелец: вложенный захват, не ждём 2 с и не блокируем сами себя. */
        if (xSemaphoreGetMutexHolder(s_lvgl_mutex) == xTaskGetCurrentTaskHandle()) {
            s_lvgl_lock_nest++;
            return;
        }
        /* Ожидание с ограничением, чтобы зависшая задача LVGL не утянула за собой остальные. */
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "LVGL mutex timed out, proceeding without lock");
        }
    }
}

void dashboard_lvgl_unlock(void) {
    /* Отдаём mutex, только если эта задача действительно им владеет. Отдача
     * mutex FreeRTOS не владельцем (после таймаута захвата) вызывает assert. */
    if (s_lvgl_mutex &&
            xSemaphoreGetMutexHolder(s_lvgl_mutex) == xTaskGetCurrentTaskHandle()) {
        if (s_lvgl_lock_nest > 0) {
            /* Вложенный unlock: внешний захват остаётся. */
            s_lvgl_lock_nest--;
            return;
        }
        xSemaphoreGive(s_lvgl_mutex);
    }
}

/* Источник тиков LVGL: миллисекунды с момента загрузки. */
static uint32_t dashboard_lvgl_tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Выполнить fn(arg) внутри задачи LVGL и блокироваться до завершения. Вызовы сериализуются через
 * s_lvgl_call_mutex. Возвращает false, если fn равен NULL, задача LVGL недоступна или не взяла вызов
 * за LVGL_CALL_START_MS (тогда fn не выполняется вовсе).
 */
bool dashboard_lvgl_call(void (*fn)(void *arg), void *arg) {
    if (!fn) {
        return false;
    }

    /* Задача LVGL не создана (ошибка): выполнять fn в чужой задаче нельзя (малый стек, объекты LVGL
     * трогает только задача LVGL), поэтому сообщаем вызывающему об отказе. */
    if (s_lvgl_task_failed) {
        return false;
    }

    /* Уже в задаче LVGL (или LVGL ещё не запущен): выполняем напрямую. */
    if (!s_lvgl_task || xTaskGetCurrentTaskHandle() == s_lvgl_task) {
        dashboard_lvgl_lock();
        fn(arg);
        dashboard_lvgl_unlock();
        return true;
    }

    /* Без логирования: у вызывающих (usb_rx, lbm_eval) стек ~3 КБ,
     * и printf-логирование поверх lispif_restart() его переполняет. */
    if (xSemaphoreTake(s_lvgl_call_mutex, pdMS_TO_TICKS(LVGL_CALL_MUTEX_MS)) != pdTRUE) {
        s_lvgl_call_stalls++;
        return false;
    }
    xSemaphoreTake(s_lvgl_call_done, 0);

    portENTER_CRITICAL(&s_call_mux);
    s_call.fn = fn;
    s_call.arg = arg;
    s_call.state = LVGL_CALL_QUEUED;
    portEXIT_CRITICAL(&s_call_mux);

    dashboard_lvgl_req_t token = 1;
    bool sent = xQueueSend(s_lvgl_req_queue, &token, pdMS_TO_TICKS(LVGL_CALL_START_MS)) == pdTRUE;
    bool ok = false;
    for (;;) {
        if (sent && xSemaphoreTake(s_lvgl_call_done, pdMS_TO_TICKS(LVGL_CALL_START_MS)) == pdTRUE) {
            ok = true;
            break;
        }
        /* таймаут: если задача LVGL ещё не взяла вызов -- отменяем его; если уже выполняет -- ждём дальше */
        portENTER_CRITICAL(&s_call_mux);
        uint8_t st = s_call.state;
        if (st == LVGL_CALL_QUEUED) {
            s_call.state = LVGL_CALL_IDLE;
        }
        portEXIT_CRITICAL(&s_call_mux);
        if (st == LVGL_CALL_QUEUED) {
            s_lvgl_call_stalls++;
            break;
        }
        sent = true;   /* вызов идёт (его могло разбудить и старое пробуждение из очереди): ждём завершения */
    }

    xSemaphoreGive(s_lvgl_call_mutex);
    return ok;
}

/* Задача LVGL: взять текущий вызов, если он ещё ждёт (пробуждение могло остаться от отменённого вызова). */
static bool dashboard_lvgl_take_call(void (**fn)(void *), void **arg) {
    bool ok = false;
    portENTER_CRITICAL(&s_call_mux);
    if (s_call.state == LVGL_CALL_QUEUED) {
        s_call.state = LVGL_CALL_RUNNING;
        *fn = s_call.fn;
        *arg = s_call.arg;
        ok = true;
    }
    portEXIT_CRITICAL(&s_call_mux);
    return ok;
}

static void dashboard_lvgl_finish_call(void) {
    portENTER_CRITICAL(&s_call_mux);
    s_call.state = LVGL_CALL_DONE;
    portEXIT_CRITICAL(&s_call_mux);
    xSemaphoreGive(s_lvgl_call_done);
}

uint32_t dashboard_lvgl_call_stalls(void) {
    return s_lvgl_call_stalls;
}

/* Пакетный режим: пока Lisp перестраивает или обновляет экран, он шлёт много мелких
 * вызовов с некоторой работой Lisp между ними. Без этого режима каждая пауза, превышающая
 * ожидание серии, позволяет LVGL отрисовать целый кадр (десятки мс при эффектах
 * с частицами) прежде чем обслужат следующий вызов. (lv-batch 1) ... (lv-batch 0)
 * удерживает отрисовку; ограничение по времени не даёт забытому пакету заморозить
 * экран. */
#define LVGL_BATCH_MAX_US 250000
static volatile bool s_lvgl_batch = false;
/* Время старта пакета в миллисекундах (uint32_t): 64-битное чтение на 32-битном RISC-V не атомарно.
 * Сравнение через беззнаковую разность устойчиво к переполнению. */
static volatile uint32_t s_lvgl_batch_t0_ms = 0;

/* Открыть (on) или закрыть пакет с удержанием отрисовки; время старта сохраняется для таймаута. */
void dashboard_lvgl_batch(bool on) {
    if (on) {
        s_lvgl_batch_t0_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_lvgl_batch = true;
    } else {
        s_lvgl_batch = false;
    }
}

/* Статистика отрисовки для (lv-perf): время в lv_timer_handler (отрисовка +
 * flush); считается кадром, если заняло больше 1 мс. */
static uint64_t s_perf_frames = 0;
static uint64_t s_perf_sum_us = 0;
static uint64_t s_perf_max_us = 0;

/* Чтение со сбросом: число сброшенных кусков и общее время в flush-callback. */
void dashboard_lvgl_perf2(uint32_t *chunks, uint32_t *flush_us) {
    portENTER_CRITICAL(&s_perf_mux);
    uint64_t c = s_perf_chunks;
    uint64_t f = s_perf_flush_us;
    s_perf_chunks = 0;
    s_perf_flush_us = 0;
    portEXIT_CRITICAL(&s_perf_mux);
    *chunks = dashboard_sat32(c);
    *flush_us = dashboard_sat32(f);
}

/* Чтение со сбросом: число кадров, среднее/максимальное время отрисовки, время программного поворота,
 * время ожидания DMA и сброшенные килопиксели с предыдущего вызова.
 */
void dashboard_lvgl_perf(uint32_t *frames, uint32_t *avg_us, uint32_t *max_us,
        uint32_t *rot_us, uint32_t *wait_us, uint32_t *kpx) {
    portENTER_CRITICAL(&s_perf_mux);
    uint64_t f = s_perf_frames;
    uint64_t rot = s_perf_rot_us;
    uint64_t wait = s_perf_wait_us;
    uint64_t px = s_perf_px;
    uint64_t sum = s_perf_sum_us;
    uint64_t mx = s_perf_max_us;
    s_perf_rot_us = 0;
    s_perf_wait_us = 0;
    s_perf_px = 0;
    s_perf_frames = 0;
    s_perf_sum_us = 0;
    s_perf_max_us = 0;
    portEXIT_CRITICAL(&s_perf_mux);
    *rot_us = dashboard_sat32(rot);
    *wait_us = dashboard_sat32(wait);
    *kpx = dashboard_sat32(px / 1000);
    *frames = dashboard_sat32(f);
    *avg_us = f ? dashboard_sat32(sum / f) : 0;
    *max_us = dashboard_sat32(mx);
}

/* Темп кадров: период таймера обновления дисплея. Тяжёлые экраны задают
 * более низкую частоту, чтобы все изменения за один период рисовались одним кадром,
 * а не дорогим кадром на каждое изменение. */
void dashboard_lvgl_refr_period(uint32_t ms) {
    if (ms < 10) {
        ms = 10;
    }
    if (ms > 200) {
        ms = 200;
    }
    dashboard_lvgl_lock();
    lv_display_t *d = lv_display_get_default();
    lv_timer_t *t = d ? lv_display_get_refr_timer(d) : NULL;
    if (t) {
        lv_timer_set_period(t, ms);
    }
    dashboard_lvgl_unlock();
}

/* Пакет учитывается только в течение LVGL_BATCH_MAX_US, чтобы пропущенный (lv-batch 0) не заморозил интерфейс. */
static bool dashboard_lvgl_batch_active(void) {
    return s_lvgl_batch &&
            (uint32_t)((uint32_t)(esp_timer_get_time() / 1000) - s_lvgl_batch_t0_ms) < (LVGL_BATCH_MAX_US / 1000);
}

/* следующий вызов из очереди: ждём 2 мс или столько, сколько открыт пакет */
static bool dashboard_lvgl_next_req(dashboard_lvgl_req_t *req) {
    for (;;) {
        if (xQueueReceive(s_lvgl_req_queue, req, pdMS_TO_TICKS(2)) == pdTRUE) {
            return true;
        }
        if (!dashboard_lvgl_batch_active()) {
            return false;
        }
    }
}

/* Главный цикл задачи LVGL: обслуживает вызовы из очереди других задач, затем (если не открыт пакет)
 * запускает lv_timer_handler() под mutex. Спит столько, сколько запросит LVGL, ограничивая значение 1..10 мс,
 * чтобы опрос касаний и вызовы из очереди оставались отзывчивыми.
 */
static void dashboard_lvgl_task(void *arg)
{
    (void)arg;

    uint32_t wait_ms = 1;

    for (;;) {

        /* Отрисовка выполняется только на Core 0 */
        if (dashboard_lvgl_batch_active()) {
            wait_ms = 1;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        dashboard_lvgl_lock();

        int64_t r0 = esp_timer_get_time();
        wait_ms = lv_timer_handler();
        uint32_t rus = (uint32_t)(esp_timer_get_time() - r0);

        dashboard_lvgl_unlock();

        /* Реальными кадрами считаются только прогоны длиннее 1 мс */
        if (rus > 1000) {
            portENTER_CRITICAL(&s_perf_mux);

            s_perf_frames++;
            s_perf_sum_us += rus;

            if (rus > s_perf_max_us) {
                s_perf_max_us = rus;
            }

            portEXIT_CRITICAL(&s_perf_mux);
        }

        /* Запасной путь включения подсветки */
        if (s_bl_release_pending) {
            vTaskDelay(pdMS_TO_TICKS(20));
            dashboard_bl_release_if_pending();
        }

        if (wait_ms < 1) {
            wait_ms = 1;
        }

        if (wait_ms > 10) {
            wait_ms = 10;
        }

        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}


/*
 * Core 1:
 * Lisp requests + выполнение LVGL вызовов
 */
static void dashboard_lvgl_call_task(void *arg)
{
    (void)arg;

    uint32_t wait_ms = 1;

    for (;;) {

        dashboard_lvgl_req_t req;

        if (xQueueReceive(
                s_lvgl_req_queue,
                &req,
                pdMS_TO_TICKS(wait_ms)) == pdTRUE) {

            /*
             * Lisp выдаёт вызовы LVGL один за другим.
             * Обрабатываем серию на Core 1.
             */
            int64_t burst_end = esp_timer_get_time() + 8000;

            do {

                void (*fn)(void *) = NULL;
                void *fn_arg = NULL;

                if (dashboard_lvgl_take_call(&fn, &fn_arg)) {

                    /*
                     * Сам LVGL всё равно сериализуем mutex'ом.
                     * Но CPU-время обработки очереди теперь
                     * выполняется на Core 1.
                     */
                    dashboard_lvgl_lock();

                    fn(fn_arg);

                    dashboard_lvgl_unlock();

                    dashboard_lvgl_finish_call();
                }

            } while (
                (dashboard_lvgl_batch_active() ||
                 esp_timer_get_time() < burst_end) &&
                dashboard_lvgl_next_req(&req)
            );
        }

        wait_ms = 1;
    }
}


/* Выбрать самый большой встроенный шрифт Montserrat, не превышающий size; если размер не включён в lv_conf,
 * используется шрифт LVGL по умолчанию.
 */
static const lv_font_t *font_or_default(int size) {
    const lv_font_t *f = LV_FONT_DEFAULT;
#if LV_FONT_MONTSERRAT_16
    if (size >= 16) f = &lv_font_montserrat_16;
#endif
#if LV_FONT_MONTSERRAT_20
    if (size >= 20) f = &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_24
    if (size >= 24) f = &lv_font_montserrat_24;
#endif
#if LV_FONT_MONTSERRAT_48
    if (size >= 48) f = &lv_font_montserrat_48;
#endif
    return f;
}

/* ------------------------------------------------------------
 * Экран ожидания: единственный UI на C. Показывается при загрузке, пока Lisp-скрипт не
 * запущен. Находится в одном полноэкранном контейнере, поэтому lv_obj_clean() при
 * первом (пере)запуске Lisp удаляет его целиком, и скрипт стартует с
 * чистого экрана.
 * ------------------------------------------------------------ */
static lv_obj_t *idle_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
        const char *text, int32_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

/* Приветствие «UI Studio», показываемое до запуска Lisp-скрипта: три вращающихся кольца. */
/* Callback анимации: повернуть дугу на угол v. */
static void idle_rot_cb(void *obj, int32_t v) {
    lv_arc_set_rotation((lv_obj_t *)obj, v);
}

/* Нарисовать кольцо: статичная тёмная дорожка плюс цветная дуга в 'sweep' градусов, которая вращается
 * от 'from' до 'to' градусов за 'ms' миллисекунд. 222 — вертикальный центр колец.
 */
static lv_obj_t *idle_ring(lv_obj_t *parent, int32_t size, uint32_t color, int32_t sweep,
        int32_t width, int32_t from, int32_t to, uint32_t ms) {
    int32_t y = 222 - size / 2;
    /* дорожка: полный круг, который не двигается */
    lv_obj_t *tr = lv_arc_create(parent);
    lv_obj_set_size(tr, size, size);
    lv_obj_align(tr, LV_ALIGN_TOP_MID, 0, y);
    lv_arc_set_bg_angles(tr, 0, 360);
    lv_arc_set_value(tr, 0);
    lv_obj_remove_style(tr, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(tr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(tr, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(tr, lv_color_hex(0x161C26), LV_PART_MAIN);
    lv_obj_set_style_arc_width(tr, 0, LV_PART_INDICATOR);

    /* цветная часть: углы фона 0..sweep при значении 100 -> рисуется вся дуга, затем она поворачивается */
    lv_obj_t *ar = lv_arc_create(parent);
    lv_obj_set_size(ar, size, size);
    lv_obj_align(ar, LV_ALIGN_TOP_MID, 0, y);
    lv_arc_set_bg_angles(ar, 0, sweep);
    lv_arc_set_range(ar, 0, 100);
    lv_arc_set_value(ar, 100);
    lv_arc_set_rotation(ar, from);
    lv_obj_remove_style(ar, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ar, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ar, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ar, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ar, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ar, true, LV_PART_INDICATOR);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ar);
    lv_anim_set_exec_cb(&a, idle_rot_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
    return ar;
}

/* Небольшой скруглённый бейдж с текстом по центру внизу экрана ожидания. */
static void idle_pill(lv_obj_t *parent, int32_t x, const char *text, uint32_t color) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 130, 34);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, x, 430);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x121820), LV_PART_MAIN);
    lv_obj_set_style_radius(p, 17, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(p, lv_color_hex(0x243040), LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font_or_default(16), LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

static void dashboard_idle_screen(void) {
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x07090D), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0x121923), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, LV_PART_MAIN);

    /* три кольца: разная скорость и направление */
    idle_ring(root, 340, 0x12C2C2, 140, 14, 150, 150 + 360, 6000);
    idle_ring(root, 290, 0x9A5BFF, 110, 14, 330, 330 - 360, 4200);
    idle_ring(root, 240, 0xFF7A00, 100, 14, 60, 60 + 360, 3000);

    /* текст должен оставаться внутри внутреннего кольца (внутренний диаметр около 210 px) */
    idle_label(root, font_or_default(32), 0xFFFFFF, "UI Studio", 190);
    idle_label(root, font_or_default(16), 0x8B8F96, "Install Lisp", 238);
    idle_label(root, font_or_default(16), 0x5A6068, "with VESC Tool", 262);

    idle_pill(root, 262, "LVGL 9.5", 0x12C2C2);
    idle_pill(root, 408, "ESP32-P4", 0xFF7A00);
}

/* Создать дисплей LVGL, буферы, ввод касаний и задачу LVGL. Аварийно завершается при сбое
 * выделения памяти для основных объектов, так как без них ничего не работает.
 */
static void dashboard_lvgl_init(void) {
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) {
        ESP_LOGE(TAG, "LVGL mutex allocation failed");
        abort();
    }

    s_lvgl_req_queue = xQueueCreate(4, sizeof(dashboard_lvgl_req_t));
    s_lvgl_call_mutex = xSemaphoreCreateMutex();
    s_lvgl_call_done = xSemaphoreCreateBinary();
    if (!s_lvgl_req_queue || !s_lvgl_call_mutex || !s_lvgl_call_done) {
        ESP_LOGE(TAG, "LVGL call queue allocation failed");
        abort();
    }

    lv_init();

    /* В LVGL 9 на ESP-IDF нет собственного источника времени. Без этого
     * тик остаётся равным 0, lv_timer_handler() никогда не запускает таймеры
     * обновления/ввода, и ничего, созданное из Lisp, не появляется на экране. */
    lv_tick_set_cb(dashboard_lvgl_tick_cb);

    /* 128 = строка кэша L2: draw-блок PPA (CONFIG_LV_USE_PPA) пишет в
     * эти буферы по DMA и требует их выравнивания по ней (LV_DRAW_BUF_ALIGN). */
    /* Выбор размера куска буфера. Ступени измеряются в строках по 800 px * 3 байта; ступень используется, если
     * три буфера помещаются во внутреннюю RAM и остаётся запас для Lisp/WiFi/BLE. Внутренняя
     * RAM намного быстрее PSRAM для отрисовки и поворота.
     */
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool use_int = false;
    static const int tiers[] = {24, 16, 12, 8};
#if DASH_PSRAM_LINES > 0
    /* эксперимент: большие куски в PSRAM (как делает esp_lvgl_adapter, 50 строк) вместо малых во внутренней RAM */
    s_buf_bytes = (size_t)800 * DASH_PSRAM_LINES * 3;
    int_free = 0;
#endif
    /* Ступень в 24 строки оставляет большой запас, меньшие ступени — только 24 КБ. */
    for (unsigned i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        size_t b = (size_t)800 * tiers[i] * 3;
        size_t reserve = tiers[i] == 24 ? (size_t)LVGL_INTERNAL_RESERVE : (size_t)(24 * 1024);
        if (int_free > 3 * b + reserve && int_big > b + 4096) {
            s_buf_bytes = b;
            use_int = true;
            break;
        }
    }
    ESP_LOGW(TAG, "LVGL buffers: internal RAM free %u (largest block %u) -> %s, chunk %u bytes", (unsigned)int_free,
            (unsigned)int_big, use_int ? "internal" : "PSRAM", (unsigned)s_buf_bytes);
    /* Предпочитаем внутреннюю RAM, пригодную для DMA; любой буфер, который не удалось выделить (или все, когда режим PSRAM
     * принудительный), выделяется ниже в PSRAM.
     */
    if (use_int) {
        s_lv_buf1 = heap_caps_aligned_alloc(128, LVGL_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_lv_buf2 = heap_caps_aligned_alloc(128, LVGL_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_rot_buf = heap_caps_aligned_alloc(128, LVGL_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_lv_buf1 || !s_lv_buf2 || !s_rot_buf) {
            /* Часть буферов не выделилась: освобождаем выделенные и берём все три из PSRAM,
             * чтобы не смешивать внутреннюю RAM и PSRAM. */
            heap_caps_free(s_lv_buf1);
            heap_caps_free(s_lv_buf2);
            heap_caps_free(s_rot_buf);
            s_lv_buf1 = NULL;
            s_lv_buf2 = NULL;
            s_rot_buf = NULL;
        }
    }
    if (!s_lv_buf1) {
        s_lv_buf1 = heap_caps_aligned_alloc(128, LVGL_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    }
    if (!s_lv_buf2) {
        s_lv_buf2 = heap_caps_aligned_alloc(128, LVGL_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    }
    if (!s_rot_buf) {
        s_rot_buf = heap_caps_aligned_alloc(128, LVGL_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    }

    /* Реальное размещение определяем по указателям, а не по намерению. */
    s_bufs_internal = s_lv_buf1 && s_lv_buf2 && s_rot_buf &&
            esp_ptr_internal(s_lv_buf1) && esp_ptr_internal(s_lv_buf2) && esp_ptr_internal(s_rot_buf);

    /* Клиент PPA для аппаратного поворота; если зарегистрировать его не удалось, используется программный поворот. */
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    s_ppa_ok = ppa_register_client(&ppa_cfg, &s_ppa_srm) == ESP_OK;
    ESP_LOGI(TAG, "PPA rotation: %s", s_ppa_ok ? "on" : "off (software)");
    s_ppa_done = xSemaphoreCreateBinary();
    if (s_ppa_ok && s_ppa_done) {
        ppa_event_callbacks_t pcb = { .on_trans_done = dashboard_ppa_done_cb };
        ppa_client_register_event_callbacks(s_ppa_srm, &pcb);
    }
    /* Тройная буферизация: нужны три буфера кадра, PPA и семафоры. Иначе прежний путь (один буфер + копия). */
    s_triple = DASH_TRIPLE_FB && s_fb[0] && s_fb[1] && s_fb[2] && s_ppa_ok && s_ppa_done && s_vsync_sem;
    ESP_LOGW(TAG, "frame buffers: %s", s_triple ? "triple (tear-free, PPA straight into the frame)" : "single");

    if (!s_lv_buf1 || !s_lv_buf2 || !s_rot_buf) {
        ESP_LOGE(TAG, "LVGL draw buffers allocation failed");
        abort();
    }

    /* Создаётся с родным размером панели; поворот делает его логическим 800x480. */
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_flush_cb(s_display, dashboard_lvgl_flush_cb);
    lv_display_set_flush_wait_cb(s_display, dashboard_lvgl_flush_wait);
    lv_display_set_buffers(s_display, s_lv_buf1, s_lv_buf2, LVGL_BUF_BYTES,
            LV_DISPLAY_RENDER_MODE_PARTIAL);
    /* Режим PARTIAL render + двойные буферы: LVGL рисует один кусок, пока предыдущий отправляется по DMA. */
    lv_display_set_rotation(s_display, (lv_display_rotation_t)LCD_ROTATION);

    /* На самом экране нет прокрутки, поэтому свайпы доходят до Lisp как жесты. */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Показывается до запуска Lisp-скрипта; удаляется при первом (пере)запуске Lisp. */
    dashboard_idle_screen();

    /* Запасной вариант удержания подсветки при включении питания (см. s_bl_hold). */
    lv_timer_t *blt = lv_timer_create(dashboard_bl_hold_timeout, BL_HOLD_MAX_MS, NULL);
    lv_timer_set_repeat_count(blt, 1);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, dashboard_lvgl_touch_cb);
    lv_timer_t *touch_read_timer = lv_indev_get_read_timer(s_indev);
    if (touch_read_timer) {
        /* опрос касаний каждые 8 мс (по умолчанию 33 мс — ощущается вялым) */
        lv_timer_set_period(touch_read_timer, 8);
    }

    /* Задача LVGL: приоритет 8, стек 12 КБ (LVGL и виджеты, созданные из Lisp, требуют большого стека). */
if (xTaskCreatePinnedToCore(
        dashboard_lvgl_task,
        "lvgl",
        12288,
        NULL,
        8,
        &s_lvgl_task,
        0) != pdPASS) {

    s_lvgl_task = NULL;
    s_lvgl_task_failed = true;

    ESP_LOGE(TAG, "LVGL render task creation failed");

    dashboard_backlight_release();
    return;
}


/* Core 1 — Lisp/LVGL command processing */
if (xTaskCreatePinnedToCore(
        dashboard_lvgl_call_task,
        "lvgl_call",
        12288,
        NULL,
        8,
        &s_lvgl_call_task,
        1) != pdPASS) {

    s_lvgl_call_task = NULL;

    ESP_LOGE(TAG, "LVGL call task creation failed");

    if (s_lvgl_task != NULL) {
        vTaskDelete(s_lvgl_task);
        s_lvgl_task = NULL;
    }

    dashboard_backlight_release();
    return;
}

ESP_LOGI(TAG,
         "LVGL workload split: render/core0 + calls/core1");

    ESP_LOGI(TAG, "LVGL task pinned to core %d", DASHBOARD_LVGL_CORE);
}

/* Запустить копирование в панель и вернуться; s_flush_done отдаётся, когда
 * DMA завершит работу. При ошибке он отдаётся сразу, чтобы никто его не ждал. */
/* (Координаты включающие, поэтому для API панели добавляется +1.) */
bool hw_p4_dashboard_flush_start(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const void *pixels) {
#if !SOC_MIPI_DSI_SUPPORTED
    (void)x1; (void)y1; (void)x2; (void)y2; (void)pixels;
    return false;
#else
    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, 0);
    }
    if (!s_panel || !pixels || x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 ||
            x2 >= LCD_H_RES || y2 >= LCD_V_RES ||
            esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, pixels) != ESP_OK) {
        if (s_flush_done) {
            xSemaphoreGive(s_flush_done);
        }
        return false;
    }
    return true;
#endif
}

/* Блокирующий вариант: нарисовать прямоугольник и ждать (макс. 1 с) завершения DMA.
 * Используется для прямой отрисовки вне flush-callback LVGL.
 */
bool hw_p4_dashboard_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const void *pixels) {
#if !SOC_MIPI_DSI_SUPPORTED
    (void)x1; (void)y1; (void)x2; (void)y2; (void)pixels;
    return false;
#else
    /* s_flush_done общий с путём LVGL: пока работает задача LVGL, блокирующий flush мог бы украсть
     * её токен завершения DMA или испортить его, поэтому отказываем. */
    if (s_lvgl_task) {
        return false;
    }
    if (!s_panel || !pixels || x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 ||
            x2 >= LCD_H_RES || y2 >= LCD_V_RES) {
        return false;
    }

    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, 0);
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, pixels);
    if (err != ESP_OK) {
        return false;
    }

    if (s_flush_done && xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    return true;
#endif
}

/* ------------------------------------------------------------
 * Быстрая очистка образа LispBM
 *
 * После каждой пересборки прошивки меняется SHA приложения, и lispif_restart()
 * перестраивает образ LispBM, записывая 0xFFFFFFFF поверх каждого слова образа.
 * flash_helper_write_code() обрабатывает каждое неочищенное слово полным
 * стиранием сектора 4 КБ + перезаписью, поэтому устаревший образ означает сотни стираний
 * секторов внутри usb_rx -> Stream/Upload/Erase в VESC Tool завершаются по таймауту
 * (видно в core dump: usb_rx в image_write -> erase_sector).
 *
 * Здесь, перед lispif_init(), выполняется та же очистка блочным стиранием
 * (секунды вместо минут), только если прошивка изменилась. Сам
 * сохранённый Lisp-код остаётся; стирается только область образа после него.
 * ------------------------------------------------------------ */
/* True, если диапазон читается как стёртый. Чтение напрямую из flash, а не
 * через mmap, чтобы устаревший кэш не скрыл данные. */
static bool dashboard_range_erased(const esp_partition_t *part, uint32_t off, uint32_t len) {
    /* окно чтения 256 байт */
    uint32_t buf[64];
    while (len > 0) {
        uint32_t n = len > sizeof(buf) ? sizeof(buf) : len;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) {
            return false;
        }
        const uint8_t *b = (const uint8_t *)buf;
        for (uint32_t i = 0; i < n; i++) {
            if (b[i] != 0xFF) {
                return false;
            }
        }
        off += n;
        len -= n;
    }
    return true;
}

/* Стереть область образа LispBM (всё в разделе "lisp" после
 * сохранённого кода и импортов), сохранив сам код. */
/* (Доступ к flash по сырым смещениям раздела; сектор, общий с кодом,
 * стирается, а байты кода записываются обратно.)
 */
/* Возвращает true, если очистка прошла без ошибок (или стирать было нечего); при ошибке стирания/записи
 * операция прерывается и возвращается false. */
static bool dashboard_erase_lisp_image(const esp_partition_t *part) {
    uint8_t *raw = flash_helper_code_data_raw(CODE_IND_LISP);
    if (raw) {
        /* Тот же расчёт начала образа, что и в lispif_restart(). */
        const uint8_t *code = flash_helper_code_data_ptr(CODE_IND_LISP);
        uint32_t code_len = flash_helper_code_size(CODE_IND_LISP);
        uintptr_t img = code ? (uintptr_t)code + code_len + 32 : (uintptr_t)raw + 32;
        img &= ~(uintptr_t)0xF;
        uint32_t img_off = (uint32_t)(img - (uintptr_t)raw);
        uint32_t sec = part->erase_size;
        uint32_t full_start = ((img_off + sec - 1) / sec) * sec;

        /* Частичный сектор, общий с кодом: одно стирание, байты кода сохраняются. */
        if (img_off % sec) {
            uint32_t sstart = full_start - sec;
            bool dirty = !dashboard_range_erased(part, img_off, full_start - img_off);
            if (dirty) {
                uint32_t keep = img_off - sstart;
                uint8_t *buf = malloc(keep ? keep : 1);
                if (!buf) {
                    /* Без копии кода стирать общий сектор нельзя: код был бы потерян. */
                    ESP_LOGE(TAG, "Lisp image clean: no memory for code backup, aborted");
                    return false;
                }
                memcpy(buf, raw + sstart, keep);
                esp_err_t e = esp_partition_erase_range(part, sstart, sec);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "Lisp image clean: erase failed (%s), aborted", esp_err_to_name(e));
                    free(buf);
                    return false;
                }
                if (keep) {
                    e = esp_partition_write(part, sstart, buf, keep);
                    if (e != ESP_OK) {
                        ESP_LOGE(TAG, "Lisp image clean: code restore write failed (%s), aborted",
                                esp_err_to_name(e));
                        free(buf);
                        return false;
                    }
                }
                free(buf);
            }
        }

        /* Остальные полные секторы: стираем только грязные, объединяя их в
         * крупные диапазоны, чтобы драйвер flash мог использовать блочные стирания по 64 КБ. */
        uint32_t run_start = 0, run_len = 0, erased = 0;
        for (uint32_t off = full_start; off < part->size; off += sec) {
            bool dirty = !dashboard_range_erased(part, off, sec);
            if (dirty) {
                if (!run_len) run_start = off;
                run_len += sec;
            } else if (run_len) {
                esp_err_t e = esp_partition_erase_range(part, run_start, run_len);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "Lisp image clean: erase failed (%s), aborted", esp_err_to_name(e));
                    return false;
                }
                erased += run_len;
                run_len = 0;
            }
        }
        if (run_len) {
            esp_err_t e = esp_partition_erase_range(part, run_start, run_len);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "Lisp image clean: erase failed (%s), aborted", esp_err_to_name(e));
                return false;
            }
            erased += run_len;
        }
        ESP_LOGI(TAG, "Lisp image area cleaned: %u KB erased",
                (unsigned)(erased / 1024));
    }
    return true;
}

/* CRC сохранённого кода + импортов, чтобы заметить новый Upload. */
static uint32_t dashboard_code_crc(void) {
    const uint8_t *code = flash_helper_code_data_ptr(CODE_IND_LISP);
    uint32_t len = flash_helper_code_size(CODE_IND_LISP);
    if (!code || len == 0) {
        return 0;
    }
    return esp_rom_crc32_le(0, code, len);
}

/* ------------------------------------------------------------
 * Устаревший образ после Upload
 *
 * При быстрой загрузке скрипт сохраняет своё окружение (image-save), и
 * прошивка затем запускает `main` из этого образа вместо разбора кода.
 * Если загружена новая программа, а старый образ уцелел, СТАРЫЙ main
 * продолжил бы работать с НОВЫМИ импортами. Поэтому при каждом перезапуске CRC
 * сохранённого кода сравнивается с тем, из которого был создан образ; если
 * они различаются, а `main` пришёл из образа, образ стирается, а
 * LispBM перезапускается один раз, и он разбирает новый код.
 * ------------------------------------------------------------ */
/* Работает в собственной задаче: lispif_restart() (а значит и callback ext-load)
 * может выполняться в usb_rx, у которого всего 3 КБ стека. */
static void dashboard_image_check_task(void *arg) {
    bool main_found = arg != NULL;
    bool stale = false;
    bool crc_changed = false;
    uint32_t crc = dashboard_code_crc();
    nvs_handle_t h;
    if (crc != 0 && nvs_open("p4dash", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t stored = 0;
        bool known = nvs_get_u32(h, "code_crc", &stored) == ESP_OK;
        if (!known || stored != crc) {
            crc_changed = true;
            stale = main_found;
            if (!stale) {
                /* Стирать нечего: новый CRC можно записать сразу. */
                nvs_set_u32(h, "code_crc", crc);
                nvs_commit(h);
            }
        }
        nvs_close(h);
    }

    if (stale) {
        vTaskDelay(pdMS_TO_TICKS(50));
        /* lispif_stop() синхронный: ждёт остановки задачи вычислений (до ~2 с) и при необходимости
         * убивает её, поэтому к стиранию flash образ уже никем не используется. */
        lispif_stop();
        const esp_partition_t *part = esp_partition_find_first(
                ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "lisp");
        bool erased_ok = false;
        if (part) {
            erased_ok = dashboard_erase_lisp_image(part);
        }
        if (erased_ok && crc_changed) {
            /* CRC сохраняем только ПОСЛЕ успешного стирания, иначе при сбое проверка не повторится. */
            nvs_handle_t h2;
            if (nvs_open("p4dash", NVS_READWRITE, &h2) == ESP_OK) {
                nvs_set_u32(h2, "code_crc", crc);
                nvs_commit(h2);
                nvs_close(h2);
            }
        }
        if (erased_ok) {
            ESP_LOGI(TAG, "Lisp image was older than the code: erased, restarting");
        } else {
            ESP_LOGE(TAG, "Lisp image erase failed, restarting without CRC update");
        }
        /* Перезапуск нужен в любом случае: lispif_stop() уже остановил LispBM. */
        lispif_restart(true, true);
    }
    vTaskDelete(NULL);
}

/* Точка входа со стороны Lisp: запускает задачу проверки (arg 1 = 'main' загружен из
 * образа).
 */
void dashboard_check_stale_image(bool main_found) {
    if (xTaskCreate(dashboard_image_check_task, "img_check", 4096,
            main_found ? (void *)1 : NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "img_check task creation failed");
    }
}

/* Проверка при загрузке: если сборка прошивки (ELF SHA-256) отличается от сохранённой в NVS,
 * один раз стереть устаревший образ LispBM быстрым блочным стиранием и запомнить новый SHA.
 */
static void dashboard_prepare_lisp_image(void) {
    const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "lisp");
    if (!part) {
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t stored[32] = {0};
    size_t stored_len = sizeof(stored);
    nvs_handle_t h;
    bool same_fw = false;
    if (nvs_open("p4dash", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "img_sha", stored, &stored_len) == ESP_OK &&
            stored_len == sizeof(stored) &&
            memcmp(stored, app->app_elf_sha256, sizeof(stored)) == 0) {
        same_fw = true;
    }

    if (!same_fw) {
        /* SHA запоминаем только если очистка удалась, иначе она повторится при следующей загрузке. */
        if (dashboard_erase_lisp_image(part)) {
            nvs_set_blob(h, "img_sha", app->app_elf_sha256, sizeof(stored));
            nvs_commit(h);
        }
    }
    nvs_close(h);
}

/* Порядок инициализации оборудования: подсветка (тёмная), LDO SD, панель, тач, LVGL, расширения Lisp. */
void hw_init(void) {
    /* Сначала подсветка, чтобы она была включена, даже если инициализация панели не удалась. */
    dashboard_backlight_init();
    dashboard_sd_init();

    dashboard_display_init();

    if (!dashboard_touch_init()) {
        ESP_LOGW(TAG, "GT911 touch is not available");
    }

    dashboard_lvgl_init();

    lispif_add_ext_load_callback(dashboard_lvgl_ext_load);

    /* Выполняется перед lispif_init() (вызывается сразу после HW_INIT_HOOK). */
    dashboard_prepare_lisp_image();
}

/* (hook-заглушка) */
void dashboard_post_lispif_init(void) {
    /* Оставлено для совместимости с HW_POST_LISPIF_HOOK.
     * Расширения Lisp для LVGL регистрируются через
     * lispif_add_ext_load_callback() в hw_init(),
     * до lispif_init()/lbm_init().
     */
}   
/* ------------------------------------------------------------------------------------------------
 * Память LVGL (используется, только если в sdkconfig выбран CONFIG_LV_USE_CUSTOM_MALLOC, иначе LVGL
 * берёт обычный malloc).
 *
 * Обычный malloc кладёт всё меньше CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 КБ) во внутреннюю RAM, пока
 * она не кончится. Объекты и стили LVGL мелкие, и интерфейс из десятков страниц может занять почти всю
 * внутреннюю RAM, оставив WiFi/BLE, драйверам и новым задачам только неприкосновенный резерв.
 * Здесь мелкие блоки LVGL идут во внутреннюю RAM (она быстрее), только пока там свободно больше
 * LVGL_INTERNAL_FLOOR; дальше -- в PSRAM. При нехватке одной области используется другая.
 * ------------------------------------------------------------------------------------------------ */
#if defined(CONFIG_LV_USE_CUSTOM_MALLOC)   /* тот же символ sdkconfig, по которому собирается компонент LVGL */

#define LVGL_INTERNAL_FLOOR   (160 * 1024)
#define LVGL_INTERNAL_MAX_BLK (16 * 1024)

void lv_mem_init(void) {
}

void lv_mem_deinit(void) {
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size) {
    void *p = NULL;
    if (size <= LVGL_INTERNAL_MAX_BLK &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > LVGL_INTERNAL_FLOOR + size) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

void *lv_realloc_core(void *p, size_t new_size) {
    if (!p) {
        return lv_malloc_core(new_size);
    }
    /* heap_caps_realloc с MALLOC_CAP_8BIT сохраняет блок на месте, если может, иначе переносит в любую область */
    return heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) {
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

#endif /* CONFIG_LV_USE_CUSTOM_MALLOC */
