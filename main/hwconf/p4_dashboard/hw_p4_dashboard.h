#ifndef MAIN_HWCONF_P4_DASHBOARD_HW_P4_DASHBOARD_H_
#define MAIN_HWCONF_P4_DASHBOARD_HW_P4_DASHBOARD_H_

#include <stdbool.h>
#include <stdint.h>

/* Выделенный аппаратный профиль Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3. */
#define HW_NAME       "P4 DASHBOARD"
#define HW_TARGET     "esp32p4_dashboard"
#define HW_NO_UART
#define HW_INIT_HOOK()          hw_init()
#define HW_POST_LISPIF_HOOK()   dashboard_post_lispif_init()
/* Быстрая загрузка: hw_init() + запуск Lisp сразу после NVS, до BLE, WiFi,
 * SD и логирования (как у дисплеев VESC, например vdisp_900). */
#define HW_EARLY_LBM_INIT

/* Распределение ядер CPU:
    Ядро 1: стек дисплея Dashboard (задача LVGL + опрос тачскрина/отрисовка).*/

#define DASHBOARD_LVGL_CORE     1

/* Разрешение панели в её родной портретной ориентации. */
#define LCD_H_RES               480
#define LCD_V_RES               800
/* Поворот экрана: 0 = родной портрет 480x800, 1 = 90 град. (альбомная 800x480),
 * 2 = 180, 3 = 270 (альбомная, с другой стороны). */
#define LCD_ROTATION            1
#define LCD_RST_GPIO            27
#define LCD_BACKLIGHT_GPIO      26
/* Подсветка на этой плате активна по НИЗКОМУ уровню (подтверждено на железе и в
 * Waveshare BSP, который управляет ею инвертированным PWM). */
#define LCD_BACKLIGHT_ON_LEVEL  0

/* Канал MIPI-DSI: скорость на линию (Мбит/с) и пиксельная частота (МГц). */
#define LCD_LANE_MBPS           500
#define LCD_DPI_CLOCK_MHZ       30

/* Шина I2C платы / GT911. Плата не выводит на P4 отдельный touch INT. */
/* Контроллер касаний GT911: порт I2C, выводы и два возможных адреса I2C. */
#define TOUCH_I2C_PORT          0
#define TOUCH_SDA_GPIO          7
#define TOUCH_SCL_GPIO          8
#define TOUCH_RST_GPIO          23
#define TOUCH_INT_GPIO          (-1)
#define TOUCH_I2C_FREQ_HZ      400000
#define TOUCH_ADDR_PRIMARY      0x5D
#define TOUCH_ADDR_SECONDARY    0x14

/* CAN (примечание: GPIO37/38 также используются как консоль журнала UART0) */
#define CAN_TX_GPIO_NUM         37
#define CAN_RX_GPIO_NUM         38

// SD-карта
#define SD_PIN_MOSI             44   // SD CMD
#define SD_PIN_MISO             39   // SD D0
#define SD_PIN_SCK              43   // SD CLK
#define SD_PIN_CS               42   // SD D3

// ADC
// Реально свободны только 3 канала ADC1: GPIO16-19 заняты
// мостом C6, GPIO23 — линия сброса тачскрина, а GPIO49/50 находятся на ADC unit 2,
// который main/adc.c не поддерживает (только ADC1) и которого на этой цели даже нет
// как ADC1_CHANNEL_8/9 (у ADC1 ESP32-P4 только каналы
// 0-7). lispif_vesc_extensions.c безусловно читает все 5 HW_ADC_CHx, поэтому
// CH3/CH4 ссылаются обратно на CH0, а не остаются неопределёнными (иначе подтянулся бы
// умолчательный ADC1_CHANNEL_3 из hw.h на GPIO19, занимая вывод моста C6).
/* Карта каналов ADC (см. примечание выше); CH3/CH4 — упомянутые там алиасы. */
#define HW_ADC_CH0				ADC1_CHANNEL_4
#define HW_ADC_CH1				ADC1_CHANNEL_5
#define HW_ADC_CH2				ADC1_CHANNEL_6
#define HW_ADC_CH3				ADC1_CHANNEL_7
#define HW_ADC_CH4				ADC1_CHANNEL_4


/* Запас под расширения моста LVGL (lv-*) сверх стандартного набора
 * (bridge/ регистрирует около 570; см. bridge/README.md). */
#ifndef USER_EXTENSION_STORAGE_SIZE
#define USER_EXTENSION_STORAGE_SIZE 768
#endif

/* Точки входа аппаратной части, локальные для dashboard. */
/* Вызывается через HW_INIT_HOOK / HW_POST_LISPIF_HOOK при старте. */
void hw_init(void);
void dashboard_post_lispif_init(void);
void dashboard_lvgl_ext_load(bool main_found);
/* Стереть образ быстрой загрузки, созданный из более старого кода (вызывается при перезапуске Lisp). */
void dashboard_check_stale_image(bool main_found);
/* Захватить/освободить мьютекс LVGL для кода вне задачи LVGL. */
void dashboard_lvgl_lock(void);
void dashboard_lvgl_unlock(void);
/* Выполнить fn(arg) внутри задачи LVGL и дождаться завершения.
 * Весь доступ к LVGL из Lisp-расширений должен идти через это. */
bool dashboard_lvgl_call(void (*fn)(void *arg), void *arg);
/* Пакетный режим: пока включён, перерисовка откладывается до его выключения. */
void dashboard_lvgl_batch(bool on);
void dashboard_lvgl_refr_period(uint32_t ms);
/* Диагностика: расположение буфера отрисовки (внутренняя RAM или PSRAM), состояние поворота PPA,
 * свободная память и статистика времени рендеринга.
 */
bool dashboard_lvgl_bufs_internal(void);
bool dashboard_lvgl_ppa_on(void);
int32_t dashboard_lvgl_ppa_status(void);
uint32_t dashboard_lvgl_internal_free(void);
uint32_t dashboard_lvgl_psram_free(void);
void dashboard_lvgl_perf2(uint32_t *chunks, uint32_t *flush_us);
void dashboard_lvgl_perf(uint32_t *frames, uint32_t *avg_us, uint32_t *max_us,
        uint32_t *rot_us, uint32_t *wait_us, uint32_t *kpx);

/* Яркость подсветки, 0 (выкл.) .. 100 (полная). PWM на LCD_BACKLIGHT_GPIO. */
void dashboard_backlight_percent(int percent);
void dashboard_backlight_request(int percent);
int dashboard_backlight_get(void);

/* Вызывается портом LVGL; реализация чисто аппаратная. */
bool hw_p4_dashboard_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const void *pixels);
bool hw_p4_dashboard_flush_start(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const void *pixels);

#endif /* MAIN_HWCONF_P4_DASHBOARD_HW_P4_DASHBOARD_H_ */
