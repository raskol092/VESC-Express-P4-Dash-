/*
 * lvgl_bridge.c -- точка входа моста LispBM <-> LVGL.
 *
 * Сам мост находится в bridge/ (см. bridge/README.md). Этот файл делает лишь то,
 * что специфично для старта прошивки.
 */
#include "lvgl_bridge.h"
#include "bridge/lvbr.h"

#include "lispbm.h"
#include "hw_p4_dashboard.h"
#include "esp_log.h"
#include "commands.h"
#include "flash_helper.h"
#include "buffer.h"

#include <string.h>

static const char *TAG = "lvgl_bridge";

/* ------------------------------------------------------------
 * Встроенные (bundled) импорты
 *
 * VESC Tool удаляет строки (import "file" 'sym) из загружаемой программы и
 * дописывает файлы после исходника вместе с таблицей (name, offset, len).
 * Прошивка должна привязать каждую запись до запуска программы. Штатный main/lispif.c
 * привязывает их лишь при вычислении строки import, чего никогда не происходит,
 * так как этих строк уже нет, поэтому dashboard привязывает их здесь
 * (тот же формат таблицы, что у ext_import).
 * Выполняется из callback загрузки расширений, пока вычислитель приостановлен.
 * ------------------------------------------------------------ */
/* Возвращает, сколько встроенных файлов привязано (0, если их нет).
 * Раскладка после 8-байтового заголовка: текст исходника, NUL, u16 count, затем для каждого
 * импорта: имя с завершающим NUL, i32 offset, i32 len (offset отсчитывается от данных).
 */
static int bind_bundled_imports(void) {
	const char *code_data = (const char*)flash_helper_code_data_raw(CODE_IND_LISP);
	int32_t code_len = (int32_t)flash_helper_code_size_raw(CODE_IND_LISP);
	if (!code_data || code_len <= 8) {
		return 0;
	}
	/* пропустить 8-байтовый заголовок перед Lisp-кодом */
	code_data += 8;
	code_len -= 8;

	int32_t code_chars = (int32_t)strnlen(code_data, (size_t)code_len);
	if (code_len <= code_chars + 3) {
		return 0;
	}

	/* таблица начинается сразу после NUL, завершающего текст исходника */
	int32_t ind = code_chars + 1;
	uint16_t num_imports = buffer_get_uint16((const uint8_t*)code_data, &ind);
	if (num_imports == 0 || num_imports >= 500) {
		return 0;
	}

	int bound = 0;
	for (int i = 0; i < num_imports; i++) {
		if (ind >= code_len) {
			break;
		}
		const char *name = code_data + ind;
		ind += (int32_t)strnlen(name, (size_t)(code_len - ind)) + 1;
		int32_t offset = buffer_get_int32((const uint8_t*)code_data, &ind);
		int32_t len = buffer_get_int32((const uint8_t*)code_data, &ind);

		/* игнорировать записи, указывающие за пределы области кода */
		if (offset < 0 || len < 0 || (int64_t)offset + len > (int64_t)code_len) {
			continue;
		}

		lbm_value val;
		/* отдать байты flash как константный массив (без копирования) и привязать его к имени */
		if (lbm_share_array_const(&val, (char*)(code_data + offset), (lbm_uint)len)) {
			if (lbm_define((char*)name, val)) {
				bound++;
			}
		}
	}
	return bound;
}

/* Данные, передаваемые в log_cb, который выполняется в задаче LVGL. */
typedef struct {
	bool ok;
	int imports;
	int ext_used;
	int ext_max;
} load_info_t;

/* Логирование выполняется в задаче LVGL (большой стек); вызывающий — usb_rx / задача eval с ~3 КБ. */
static void log_cb(void *arg) {
	const load_info_t *i = arg;
	ESP_LOGI(TAG, "Bundled imports bound: %d", i->imports);
	if (i->ok) {
		ESP_LOGI(TAG, "LVGL bridge ready: %u functions, %u extras, %u constants (extension table %d/%d)",
				lvbr_fn_count, lvbr_extra_count + lvbr_events_count, lvbr_const_count, i->ext_used, i->ext_max);
	} else {
		ESP_LOGE(TAG, "Some LVGL extensions could not be registered (extension table %d/%d): "
				"raise USER_EXTENSION_STORAGE_SIZE in hw_p4_dashboard.h", i->ext_used, i->ext_max);
	}
}

/* Callback загрузки расширений: привязывает встроенные импорты, регистрирует мост lv-*
 * (lvbr_load) и сообщает результат в Lisp REPL и в лог ESP.
 */
void dashboard_lvgl_ext_load(bool main_found) {
	/* удалить образ быстрой загрузки, созданный из более старого кода, до запуска программы */
	dashboard_check_stale_image(main_found);

	load_info_t info;
	info.imports = bind_bundled_imports();
	info.ok = lvbr_load();
	info.ext_used = (int)lbm_get_num_extensions();
	info.ext_max = (int)lbm_get_max_extensions();

	/* также в Lisp REPL (VESC Tool): лог ESP там не виден */
	commands_printf_lisp("[lvbr] imports bound %d, register %s, extensions %d/%d, refused: ext %u const %u, consts %u",
			info.imports, info.ok ? "ok" : "FAILED", info.ext_used, info.ext_max,
			lvbr_fail_ext, lvbr_fail_def, (unsigned)lvbr_const_count);
	commands_printf_lisp("[lvbr] draw buffers %s, PPA rotation %s, internal RAM free %u",
			dashboard_lvgl_bufs_internal() ? "internal" : "PSRAM", dashboard_lvgl_ppa_on() ? "on" : "off",
			(unsigned)dashboard_lvgl_internal_free());

	dashboard_lvgl_call(log_cb, &info);
}
