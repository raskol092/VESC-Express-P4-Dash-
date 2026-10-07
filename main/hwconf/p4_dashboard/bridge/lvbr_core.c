/*
 * lvbr_core.c -- небольшая стабильная часть моста LispBM <-> LVGL.
 *
 *  - таблица handle (с проверкой generation, одна таблица для всех видов указателей)
 *  - диспетчеризация: аргументы Lisp -> lvbr_call_t -> задача LVGL -> результат -> значение Lisp
 *  - регистрация расширений / констант / шрифтов
 *  - сброс при (пере)загрузке Lisp-программы
 *
 * Ничто здесь не знает об отдельных функциях LVGL.
 */
#include "lvbr.h"
#include "hw_p4_dashboard.h"
#include "src/core/lv_obj_private.h"   /* is_deleting: умирающему объекту новый handle не выдаётся */

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ handle */

/* Handle = (generation << 12) | индекс слота. Слот 0 никогда не используется, чтобы handle 0 мог означать nil/недействительный. */
#define HANDLE_BITS   12   /* 4095 слотов: весь интерфейс строится сразу (все страницы сетки прокрутки) */
#define GEN_BITS      15
#define GEN_MASK      ((1u << GEN_BITS) - 1)
#define HANDLE_SLOTS  (1 << HANDLE_BITS)
#define HANDLE_MASK   (HANDLE_SLOTS - 1)

/* Generation 1..32767 увеличивается при освобождении, поэтому устаревший handle удалённого объекта не проходит проверку в lvbr_handle_get(). */
typedef struct {
	void   *p;
	uint8_t kind;   /* lvbr_hkind_t, NONE = свободный слот */
	uint16_t gen;   /* 1..32767 (12 + 15 = 27 бит: handle влезает в малое целое LispBM) */
} lvbr_slot_t;

/* Используется только из задачи LVGL, поэтому блокировка не нужна. */
static lvbr_slot_t s_slot[HANDLE_SLOTS];
static unsigned    s_rover = 1;

static void on_obj_delete(lv_event_t *e);

/* Выделяет слот (по кругу от s_rover, чтобы освобождённые слоты не переиспользовались сразу). Возвращает 0, когда таблица заполнена. */
int32_t lvbr_handle_new(lvbr_hkind_t kind, void *ptr) {
	for (unsigned n = 0; n < HANDLE_SLOTS - 1; n++) {
		unsigned i = 1 + ((s_rover - 1 + n) % (HANDLE_SLOTS - 1));
		if (s_slot[i].kind == LVBR_HK_NONE) {
			if (s_slot[i].gen == 0) {
				s_slot[i].gen = 1;
			}
			s_slot[i].kind = (uint8_t)kind;
			s_slot[i].p = ptr;
			s_rover = i + 1;
			return ((int32_t)s_slot[i].gen << HANDLE_BITS) | (int32_t)i;
		}
	}
	return 0;
}

/* Преобразует handle в указатель; NULL, если kind или generation не совпадают (устаревший handle или неверный тип). */
void *lvbr_handle_get(lvbr_hkind_t kind, int32_t h) {
	unsigned i = (unsigned)h & HANDLE_MASK;
	unsigned g = ((unsigned)h >> HANDLE_BITS) & GEN_MASK;
	/* биты выше поля generation должны быть нулевыми, иначе это не наш handle */
	if (h <= 0 || ((unsigned)h >> (HANDLE_BITS + GEN_BITS)) != 0 || i == 0 || s_slot[i].kind != kind || s_slot[i].gen != g) {
		return NULL;
	}
	return s_slot[i].p;
}

/* Освобождает слот и увеличивает его generation (после 32767 переходит к 1, никогда не 0), чтобы старые копии handle стали недействительными. */
void lvbr_handle_free(int32_t h) {
	unsigned i = (unsigned)h & HANDLE_MASK;
	unsigned g = ((unsigned)h >> HANDLE_BITS) & GEN_MASK;
	if (h <= 0 || ((unsigned)h >> (HANDLE_BITS + GEN_BITS)) != 0 || i == 0 || s_slot[i].kind == LVBR_HK_NONE || s_slot[i].gen != g) {
		return;
	}
	s_slot[i].kind = LVBR_HK_NONE;
	s_slot[i].p = NULL;
	s_slot[i].gen = (uint16_t)(g % GEN_MASK + 1);
	lvbr_events_forget(h);
}

/* handle существующего указателя (любой kind, кроме OBJ, ищется перебором) или новый */
/* Линейный перебор таблицы; приемлемо для немногочисленных не-объектных handle (шрифты, series, ...). */
static int32_t handle_for(lvbr_hkind_t kind, void *p) {
	if (!p) {
		return 0;
	}
	for (unsigned i = 1; i < HANDLE_SLOTS; i++) {
		if (s_slot[i].kind == kind && s_slot[i].p == p) {
			return ((int32_t)s_slot[i].gen << HANDLE_BITS) | (int32_t)i;
		}
	}
	return lvbr_handle_new(kind, p);
}

int32_t lvbr_handle_for(lvbr_hkind_t kind, void *p) {
	if (kind == LVBR_HK_OBJ) {
		return lvbr_obj_handle((lv_obj_t *)p);
	}
	return handle_for(kind, p);
}

/* Объекты хранят свой handle в user_data LVGL: поиск за O(1), а delete callback освобождает слот при удалении объекта. */
int32_t lvbr_obj_handle(lv_obj_t *obj) {
	if (!obj) {
		return 0;
	}
	int32_t h = (int32_t)(intptr_t)lv_obj_get_user_data(obj);
	if (h && lvbr_handle_get(LVBR_HK_OBJ, h) == obj) {
		return h;
	}
	/* объект уже удаляется (события, пришедшие во время удаления): новый handle указывал бы на освобождённую
	 * память, поэтому его нет */
	if (obj->is_deleting) {
		return 0;
	}
	h = lvbr_handle_new(LVBR_HK_OBJ, obj);
	if (!h) {
		return 0;
	}
	lv_obj_set_user_data(obj, (void *)(intptr_t)h);
	lv_obj_add_event_cb(obj, on_obj_delete, LV_EVENT_DELETE, NULL);
	return h;
}

/* handle объекта без создания: действующий, либо (объект удаляется) прежнее значение из user_data. */
int32_t lvbr_obj_handle_peek(lv_obj_t *obj) {
	if (!obj) {
		return 0;
	}
	int32_t h = (int32_t)(intptr_t)lv_obj_get_user_data(obj);
	return h > 0 ? h : 0;
}

/* Хук LV_EVENT_DELETE: сбрасывает подписки на события и освобождает handle удалённого объекта. */
static void on_obj_delete(lv_event_t *e) {
	lv_obj_t *obj = lv_event_get_target_obj(e);
	int32_t h = (int32_t)(intptr_t)lv_obj_get_user_data(obj);
	lvbr_events_obj_deleted(obj);
#if LV_USE_CHART
	/* series удаляемого графика исчезнут вместе с ним: освобождаем их handle (событие DELETE приходит до деструктора) */
	if (lv_obj_check_type(obj, &lv_chart_class)) {
		for (lv_chart_series_t *sr = lv_chart_get_series_next(obj, NULL); sr; sr = lv_chart_get_series_next(obj, sr)) {
			for (unsigned i = 1; i < HANDLE_SLOTS; i++) {
				if (s_slot[i].kind == LVBR_HK_SER && s_slot[i].p == sr) {
					lvbr_handle_free(((int32_t)s_slot[i].gen << HANDLE_BITS) | (int32_t)i);
					break;
				}
			}
		}
	}
#endif
	if (h && lvbr_handle_get(LVBR_HK_OBJ, h) == obj) {
		lvbr_handle_free(h);
	}
}

/* ---------------------------------------------------------------- стили */

/* Стили выделяются в куче и принадлежат мосту; Lisp хранит только handle. Освобождаются явно или в reset_cb(). */
int32_t lvbr_style_new(void) {
	lv_style_t *s = lv_malloc(sizeof(lv_style_t));
	if (!s) {
		return 0;
	}
	lv_style_init(s);
	int32_t h = lvbr_handle_new(LVBR_HK_STYLE, s);
	if (!h) {
		lv_style_reset(s);
		lv_free(s);
	}
	return h;
}

/* Удалённые Lisp-ом стили: объекты могут всё ещё ссылаться на lv_style_t, поэтому память не освобождается сразу,
 * а откладывается до reset_cb() (список односвязный, узел выделяется через lv_malloc). */
typedef struct style_zombie { struct style_zombie *next; lv_style_t *s; } style_zombie_t;
static style_zombie_t *s_zombies;

bool lvbr_style_delete(int32_t h) {
	lv_style_t *s = lvbr_handle_get(LVBR_HK_STYLE, h);
	if (!s) {
		return false;
	}
	style_zombie_t *z = lv_malloc(sizeof(style_zombie_t));
	if (!z) {
		/* нет памяти на узел: handle остаётся действительным, стиль не освобождаем (безопаснее, чем use-after-free) */
		return false;
	}
	z->s = s;
	z->next = s_zombies;
	s_zombies = z;
	lvbr_handle_free(h);
	return true;
}

/* Освобождает отложенные стили; вызывается только когда объекты, использовавшие их, уже удалены. */
static void style_zombies_free(void) {
	while (s_zombies) {
		style_zombie_t *z = s_zombies;
		s_zombies = z->next;
		lv_style_reset(z->s);
		lv_free(z->s);
		lv_free(z);
	}
}

/* ----------------------------------------------------------------- запуск */

/* Выполняет fn(arg) в задаче LVGL и блокируется до завершения (LVGL не потокобезопасен).
 * После fn освобождаются регистрации событий удалённых объектов (здесь рассылки событий точно нет). */
typedef struct { void (*fn)(void *); void *arg; } run_t;
static void run_cb(void *a) {
	run_t *r = a;
	r->fn(r->arg);
	lvbr_events_gc();
}
bool lvbr_run(void (*fn)(void *), void *arg) {
	run_t r = { fn, arg };
	return dashboard_lvgl_call(run_cb, &r);
}

/* Проверки перед вызовом функции LVGL (задача LVGL). */

/* Класс виджета по префиксу имени функции: lv_<widget>_* принимает только объект этого класса.
 * lv_obj_* не ограничиваются. Префикс заканчивается на '_', поэтому lv_image_ не совпадает с lv_imagebutton_, а lv_led_ с lv_line_. */
typedef struct { const char *prefix; const lv_obj_class_t *cls; } widget_rule_t;
static const widget_rule_t s_widget_rules[] = {
#if LV_USE_ARC
	{ "lv_arc_", &lv_arc_class },
#endif
#if LV_USE_BAR
	{ "lv_bar_", &lv_bar_class },
#endif
#if LV_USE_SLIDER
	{ "lv_slider_", &lv_slider_class },
#endif
#if LV_USE_CHART
	{ "lv_chart_", &lv_chart_class },
#endif
#if LV_USE_TABLE
	{ "lv_table_", &lv_table_class },
#endif
#if LV_USE_LABEL
	{ "lv_label_", &lv_label_class },
#endif
#if LV_USE_IMAGE
	{ "lv_image_", &lv_image_class },
#endif
#if LV_USE_LINE
	{ "lv_line_", &lv_line_class },
#endif
#if LV_USE_SCALE
	{ "lv_scale_", &lv_scale_class },
#endif
#if LV_USE_SPINBOX
	{ "lv_spinbox_", &lv_spinbox_class },
#endif
#if LV_USE_DROPDOWN
	{ "lv_dropdown_", &lv_dropdown_class },
#endif
#if LV_USE_TEXTAREA
	{ "lv_textarea_", &lv_textarea_class },
#endif
#if LV_USE_SWITCH
	{ "lv_switch_", &lv_switch_class },
#endif
#if LV_USE_CHECKBOX
	{ "lv_checkbox_", &lv_checkbox_class },
#endif
#if LV_USE_ROLLER
	{ "lv_roller_", &lv_roller_class },
#endif
#if LV_USE_KEYBOARD
	{ "lv_keyboard_", &lv_keyboard_class },
#endif
#if LV_USE_LED
	{ "lv_led_", &lv_led_class },
#endif
#if LV_USE_CALENDAR
	{ "lv_calendar_", &lv_calendar_class },
#endif
#if LV_USE_LIST
	{ "lv_list_", &lv_list_class },
#endif
#if LV_USE_MSGBOX
	{ "lv_msgbox_", &lv_msgbox_class },
#endif
#if LV_USE_TABVIEW
	{ "lv_tabview_", &lv_tabview_class },
#endif
#if LV_USE_TILEVIEW
	{ "lv_tileview_", &lv_tileview_class },
#endif
#if LV_USE_WIN
	{ "lv_win_", &lv_win_class },
#endif
#if LV_USE_SPINNER
	{ "lv_spinner_", &lv_spinner_class },
#endif
};

/* Верхняя граница числовых аргументов (kind U: отрицательное число превращается в огромное и тоже отклоняется). */
typedef struct { const char *name; uint8_t arg; uint32_t max; } limit_rule_t;
static const limit_rule_t s_limit_rules[] = {
	{ "lv_chart_set_point_count",       1, 1024 },
	{ "lv_table_set_row_count",         1, 1024 },
	{ "lv_table_set_column_count",      1, 1024 },
	{ "lv_table_set_cell_value",        1, 1024 },   /* row */
	{ "lv_table_set_cell_value",        2, 1024 },   /* col */
	{ "lv_spinbox_set_digit_format",    1, 20 },     /* число цифр */
	{ "lv_spinbox_set_digit_format",    2, 20 },     /* позиция разделителя */
	{ "lv_scale_set_total_tick_count",  1, 1024 },
};

/* Системные слои нельзя удалять, очищать и лишать стилей из Lisp. */
static bool is_sys_layer(const lv_obj_t *o) {
	return o == lv_layer_top() || o == lv_layer_sys() || o == lv_layer_bottom();
}

/* Правила проверки функции, вычисляемые по её имени один раз при регистрации (s_fn_rule[idx]),
 * чтобы каждый вызов не сравнивал строки. */
#define RULE_WIDGET_MASK    0x1Fu   /* 1 + индекс в s_widget_rules, 0 = нет ограничения класса */
#define RULE_SYS_LAYER      0x20u   /* нельзя применять к системному слою */
#define RULE_SCREEN_LOAD    0x40u   /* аргумент должен быть экраном */
#define RULE_LIMIT          0x80u   /* есть записи в s_limit_rules */
#define RULE_REMOVE_SERIES  0x100u  /* lv_chart_remove_series: освободить handle series */

static uint16_t rule_of(const char *n) {
	uint16_t r = 0;
	for (unsigned i = 0; i < sizeof(s_widget_rules) / sizeof(s_widget_rules[0]); i++) {
		if (strncmp(n, s_widget_rules[i].prefix, strlen(s_widget_rules[i].prefix)) == 0) {
			r |= (uint16_t)(i + 1);
			break;
		}
	}
	if (strcmp(n, "lv_obj_delete") == 0 || strcmp(n, "lv_obj_clean") == 0 ||
			strcmp(n, "lv_obj_delete_delayed") == 0 || strcmp(n, "lv_obj_remove_style_all") == 0) {
		r |= RULE_SYS_LAYER;
	}
	if (strcmp(n, "lv_screen_load") == 0 || strcmp(n, "lv_screen_load_anim") == 0) {
		r |= RULE_SCREEN_LOAD;
	}
	for (unsigned i = 0; i < sizeof(s_limit_rules) / sizeof(s_limit_rules[0]); i++) {
		if (strcmp(n, s_limit_rules[i].name) == 0) {
			r |= RULE_LIMIT;
		}
	}
	if (strcmp(n, "lv_chart_remove_series") == 0) {
		r |= RULE_REMOVE_SERIES;
	}
	return r;
}

static uint16_t *s_fn_rule;   /* по lvbr_fn_t.idx; строится в lvbr_register() */

static uint16_t fn_rule(const lvbr_fn_t *f) {
	return s_fn_rule ? s_fn_rule[f->idx] : rule_of(f->name);
}

/* NULL, если вызов допустим, иначе текст ошибки. Аргументы-handle уже разрешены в указатели. */
static const char *check_call(const lvbr_fn_t *f, const lvbr_call_t *c, uint16_t rule) {
	if (f->nargs > 0 && f->arg[0] == LVBR_K_OBJ) {
		const lv_obj_t *o = c->a[0].p;
		unsigned wi = rule & RULE_WIDGET_MASK;
		if (wi && !lv_obj_check_type(o, s_widget_rules[wi - 1].cls)) {
			return "lv: wrong widget type";
		}
		if ((rule & RULE_SYS_LAYER) && is_sys_layer(o)) {
			return "lv: operation not allowed on a system layer";
		}
		/* загружать можно только экран (объект без родителя) */
		if ((rule & RULE_SCREEN_LOAD) && lv_obj_get_parent(o) != NULL) {
			return "lv: not a screen";
		}
	}

	if (rule & RULE_LIMIT) {
		for (unsigned i = 0; i < sizeof(s_limit_rules) / sizeof(s_limit_rules[0]); i++) {
			if (strcmp(f->name, s_limit_rules[i].name) == 0 && c->a[s_limit_rules[i].arg].u > s_limit_rules[i].max) {
				return "lv: value out of range";
			}
		}
	}
	return NULL;
}

/* Series принимается только вместе с графиком-владельцем: ищем её в списке series этого графика. */
static bool chart_owns_series(const lv_obj_t *chart, const void *ser) {
#if LV_USE_CHART
	lv_obj_t *ch = (lv_obj_t *)chart;
	if (!lv_obj_check_type(ch, &lv_chart_class)) {
		return false;
	}
	for (lv_chart_series_t *s = lv_chart_get_series_next(ch, NULL); s; s = lv_chart_get_series_next(ch, s)) {
		if (s == ser) {
			return true;
		}
	}
#else
	(void)chart; (void)ser;
#endif
	return false;
}

/* Выполняется в задаче LVGL: превращает аргументы-handle (целые числа) в настоящие указатели, затем вызывает реализацию. */
static void exec_cb(void *arg) {
	lvbr_call_t *c = arg;
	const lvbr_fn_t *f = c->fn;
	int ser_i = 0;
	int32_t ser_h = 0;   /* handle series среди аргументов (для lv_chart_remove_series) */

	for (int i = 0; i < f->nargs; i++) {
		lvbr_hkind_t hk = lvbr_kind_hkind((lvbr_kind_t)f->arg[i]);
		if (hk == LVBR_HK_NONE) {
			continue;
		}
		/* OBJN может быть nil (NULL); любой другой вид handle должен быть не nil и всё ещё действителен. */
		int32_t h = c->a[i].i;
		if (h == 0) {
			if (f->arg[i] == LVBR_K_OBJN) {
				c->a[i].p = NULL;
				continue;
			}
			c->err = "lv: handle is nil";
			return;
		}
		void *p = lvbr_handle_get(hk, h);
		if (!p) {
			c->err = "lv: invalid or deleted handle";
			return;
		}
		c->a[i].p = p;
		if (hk == LVBR_HK_SER) {
			ser_h = h;
			ser_i = i;
		}
	}
	uint16_t rule = fn_rule(f);
	{
		const char *e = check_call(f, c, rule);
		if (e) {
			c->err = e;
			return;
		}
	}
	/* series действительна только для своего графика (первый аргумент); тип графика уже проверен выше */
	if (ser_h && (f->arg[0] != LVBR_K_OBJ || !chart_owns_series(c->a[0].p, c->a[ser_i].p))) {
		c->err = "lv: series does not belong to this chart";
		return;
	}
	f->impl(c);
	/* после удаления series из графика её handle должен стать недействительным */
	if (ser_h && !c->err && (rule & RULE_REMOVE_SERIES)) {
		lvbr_handle_free(ser_h);
	}
	/* вне рассылки событий: можно освободить регистрации объектов, удалённых этим или прошлыми вызовами */
	lvbr_events_gc();
}

/* ---------------------------------------------------- преобразование аргументов */

/* Помощники для расширений, написанных вручную (задача eval): проверяют один аргумент и при ошибке устанавливают причину ошибки Lisp. */
bool lvbr_arg_i32(lbm_value v, int32_t *out) {
	if (!lbm_is_number(v)) {
		lbm_set_error_reason("lv: expected a number");
		return false;
	}
	*out = lbm_dec_as_i32(v);
	return true;
}

bool lvbr_arg_str(lbm_value v, const char **out) {
	if (!lbm_is_array_r(v)) {
		lbm_set_error_reason("lv: expected a string");
		return false;
	}
	lbm_array_header_t *h = lbm_dec_array_r(v);
	if (!h || !h->data || h->size == 0 || ((const char *)h->data)[h->size - 1] != '\0') {
		lbm_set_error_reason("lv: expected a zero terminated string");
		return false;
	}
	*out = (const char *)h->data;
	return true;
}

/* Копирует C-строку длины n в новый байтовый массив LispBM (с завершающим нулём); выполняется в задаче eval. */
static lbm_value make_string(const char *s, size_t n) {
	lbm_value res;
	if (!lbm_heap_allocate_array(&res, (lbm_uint)n + 1)) {
		return ENC_SYM_MERROR;
	}
	lbm_array_header_t *h = lbm_dec_array_rw(res);
	if (n) {
		memcpy(h->data, s, n);
	}
	((char *)h->data)[n] = '\0';
	return res;
}

/*
 * Сторона задачи eval для каждой сгенерированной функции lv-*: преобразует аргументы Lisp в lvbr_call_t (здесь нет доступа к LVGL),
 * переходит в задачу LVGL через dashboard_lvgl_call(), затем преобразует результат обратно. Структура вызова живёт на этом
 * (небольшом) стеке, что безопасно, так как мы блокируемся, пока задача LVGL не закончит с ней работу.
 */
lbm_value lvbr_dispatch(const lvbr_fn_t *f, lbm_value *args, lbm_uint argn) {
	lvbr_call_t c;
	c.fn = f;
	c.err = NULL;
	c.r.u = 0;

	if (argn != f->nargs) {
		lbm_set_error_reason("lv: wrong number of arguments");
		return ENC_SYM_EERROR;
	}

	for (unsigned i = 0; i < f->nargs; i++) {
		lbm_value v = args[i];
		switch ((lvbr_kind_t)f->arg[i]) {
		case LVBR_K_I:
		case LVBR_K_U:
			if (!lbm_is_number(v)) { lbm_set_error_reason("lv: expected a number"); return ENC_SYM_TERROR; }
			if (f->arg[i] == LVBR_K_I) { c.a[i].i = lbm_dec_as_i32(v); } else { c.a[i].u = lbm_dec_as_u32(v); }
			break;
		case LVBR_K_C:
			if (!lbm_is_number(v)) { lbm_set_error_reason("lv: expected a colour number"); return ENC_SYM_TERROR; }
			/* Цвета -- 24-битный RGB; отбрасываем любые alpha/лишние биты. */
			c.a[i].u = lbm_dec_as_u32(v) & 0xFFFFFF;
			break;
		case LVBR_K_B:
			/*
			 * bool: nil -> false, число -> не ноль, всё остальное (t, символы) -> true.
			 */
			if (lbm_is_symbol_nil(v)) { c.a[i].i = 0; }
			else if (lbm_is_number(v)) { c.a[i].i = lbm_dec_as_i32(v) != 0; }
			else { c.a[i].i = 1; }
			break;
		case LVBR_K_S:
			if (!lvbr_arg_str(v, &c.a[i].s)) { return ENC_SYM_TERROR; }
			break;
		default:   /* виды handle */
			if (lbm_is_symbol_nil(v)) { c.a[i].i = 0; }
			else if (lbm_is_number(v)) { c.a[i].i = lbm_dec_as_i32(v); }
			else { lbm_set_error_reason("lv: expected a handle"); return ENC_SYM_TERROR; }
			break;
		}
	}

	/* Синхронный переход в задачу LVGL; завершается ошибкой, если эта задача не запущена. */
	if (!dashboard_lvgl_call(exec_cb, &c)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	if (c.err) {
		lbm_set_error_reason(c.err);
		return ENC_SYM_EERROR;
	}

	switch ((lvbr_kind_t)f->ret) {
	case LVBR_K_V: return ENC_SYM_TRUE;
	/* Преобразуем результат по объявленному виду возврата; для handle 0 снова превращается в nil. */
	case LVBR_K_I: return lbm_enc_i32(c.r.i);
	case LVBR_K_U: return lbm_enc_u32(c.r.u);
	case LVBR_K_B: return c.r.i ? ENC_SYM_TRUE : ENC_SYM_NIL;
	case LVBR_K_C: return lbm_enc_i((int32_t)(c.r.u & 0xFFFFFF));
	case LVBR_K_S: return make_string(lvbr_strbuf, c.r.u <= LVBR_STR_MAX ? c.r.u : LVBR_STR_MAX);
	default:       return c.r.i ? lbm_enc_i(c.r.i) : ENC_SYM_NIL;   /* виды handle */
	}
}

/* ------------------------------------------------------------ регистрация */

/* Регистрация в LispBM сгенерированных функций, групп, написанных вручную, констант и шрифтов. */
typedef struct { const lvbr_ext_t *t; const unsigned *n; } ext_group_t;

/* Новый файл с расширениями, написанными вручную = ещё одна строка здесь. */
static const ext_group_t s_groups[] = {
	{ lvbr_extra_table,  &lvbr_extra_count  },
	{ lvbr_events_table, &lvbr_events_count },
	{ lvbr_assets_table, &lvbr_assets_count },
};

/* Таблицы имён строятся один раз и сохраняются навсегда (LispBM хранит указатели и никогда не копирует их). */
static char **s_fn_names;      /* "lv-obj-set-pos" для каждой записи lvbr_fn_table, строится один раз */
static char **s_const_names;   /* "lv_part_main" для каждой константы: LispBM читает символы в нижнем регистре */

/* LispBM хранит указатели на имена, поэтому они живут в одном постоянном блоке. */
/* Строит таблицу преобразованных имён ('_' -> '-' для функций, нижний регистр для констант) в одном пуле, выделенном через malloc. */
static char **make_names(unsigned count, const char *(*get)(unsigned), bool dashes, bool lower) {
	size_t total = 0;
	for (unsigned i = 0; i < count; i++) {
		total += strlen(get(i)) + 1;
	}
	char **tab = malloc(sizeof(char *) * count);
	char *pool = malloc(total);
	if (!tab || !pool) {
		free(tab);
		free(pool);
		return NULL;
	}
	char *w = pool;
	for (unsigned i = 0; i < count; i++) {
		tab[i] = w;
		for (const char *src = get(i); *src; src++) {
			char c = *src;
			if (dashes && c == '_') {
				c = '-';
			}
			if (lower && c >= 'A' && c <= 'Z') {
				c = (char)(c - 'A' + 'a');
			}
			*w++ = c;
		}
		*w++ = '\0';
	}
	return tab;
}

static const char *fn_name_of(unsigned i)    { return lvbr_fn_table[i]->name; }
static const char *const_name_of(unsigned i) { return lvbr_const_table[i].name; }

static bool build_names(void) {
	if (!s_fn_rule) {
		/* правила проверки по номеру функции; при нехватке памяти остаётся медленный путь по имени */
		uint16_t *r = malloc(sizeof(uint16_t) * lvbr_fn_count);
		if (r) {
			for (unsigned i = 0; i < lvbr_fn_count; i++) {
				r[lvbr_fn_table[i]->idx] = rule_of(lvbr_fn_table[i]->name);
			}
			s_fn_rule = r;
		}
	}
	if (!s_fn_names) {
		s_fn_names = make_names(lvbr_fn_count, fn_name_of, true, false);
	}
	if (!s_const_names) {
		s_const_names = make_names(lvbr_const_count, const_name_of, false, true);
	}
	return s_fn_names && s_const_names;
}

/* Малые значения помещаются в непосредственное целое (28-битный знаковый диапазон за вычетом тега), большие требуют boxed u32. */
static lbm_value enc_u32(uint32_t v) {
	return v < (1u << 27) ? lbm_enc_i((int32_t)v) : lbm_enc_u32(v);
}

/* handle шрифтов, создаются в lvbr_reset() */
/* Handle встроенных шрифтов; заполняются заново в reset_cb() после очистки таблицы handle (макс. 64 шрифта). */
static int32_t s_font_h[64];

/* Счётчики отклонённых регистраций, сообщаются при загрузке. */
unsigned lvbr_fail_ext, lvbr_fail_def;   /* отклонённые регистрации (выводятся при загрузке) */

bool lvbr_register(void) {
	bool ok = build_names();
	lvbr_fail_ext = 0;
	lvbr_fail_def = 0;

	if (!ok) {
		return false;
	}
	for (unsigned i = 0; i < lvbr_fn_count; i++) {
		if (!lbm_add_extension(s_fn_names[i], lvbr_fn_table[i]->ext)) { ok = false; lvbr_fail_ext++; }
	}
	for (unsigned g = 0; g < sizeof(s_groups) / sizeof(s_groups[0]); g++) {
		for (unsigned i = 0; i < *s_groups[g].n; i++) {
			if (!lbm_add_extension((char *)s_groups[g].t[i].lisp_name, s_groups[g].t[i].ext)) { ok = false; lvbr_fail_ext++; }
		}
	}
	for (unsigned i = 0; i < lvbr_const_count; i++) {
		if (!lbm_define(s_const_names[i], enc_u32(lvbr_const_table[i].value))) { lvbr_fail_def++; }
	}
	for (unsigned i = 0; i < lvbr_font_count && i < 64; i++) {
		if (s_font_h[i]) {
			lbm_define((char *)lvbr_font_table[i].name, lbm_enc_i(s_font_h[i]));
		}
	}
	return ok;
}

/* ------------------------------------------------------------------- сброс */

/*
 * Выполняется в задаче LVGL при каждой (пере)загрузке Lisp-программы. Порядок важен: сначала события, затем экран
 * и его объекты, затем стили (объекты должны исчезнуть раньше используемых ими стилей), затем assets, затем вся
 * таблица handle, и в конце заново создаются handle встроенных шрифтов.
 */
static void reset_cb(void *arg) {
	(void)arg;
	lvbr_events_reset();
	/* Анимации слайдов останавливаем ДО того, как читаем old: их остановка может сама удалить/подменить объекты
	 * (иначе возможно двойное удаление одного экрана). */
	lvbr_extra_reset();

	/* Новый пустой экран заменяет всё, что построила старая программа. */
	/* Сначала подставляем пустой экран, чтобы старый можно было удалить, пока он не отображается. */
	lv_obj_t *old = lv_screen_active();
	lv_obj_t *blank = lv_obj_create(NULL);
	if (blank) {
		lv_screen_load(blank);
		if (old && old != blank) {
			lv_obj_delete(old);
		}
	} else if (old) {
		/* Нет памяти на пустой экран: активный экран удалять нельзя, поэтому только очищаем его содержимое и стили.
		 * Таблицу handle всё равно сбрасываем ниже: объектов старой программы к этому моменту не остаётся
		 * (кроме самих экранов, которые удаляются в цикле ниже), а стили освобождаются безопасно. */
		lv_obj_clean(old);
		lv_obj_remove_style_all(old);
	}
	/* слои: sheet старой программы живут в top, sys и bottom; убираем детей, стили и все обработчики событий
	 * (в том числе наш on_obj_delete: handle таблица ниже сбрасывается, а lvbr_obj_handle добавит его заново),
	 * пока стили и шрифты ещё не освобождены, чтобы не осталось висячих указателей */
	lv_obj_t *layers[3] = { lv_layer_top(), lv_layer_sys(), lv_layer_bottom() };
	for (unsigned li = 0; li < 3; li++) {
		if (!layers[li]) {
			continue;
		}
		lv_obj_clean(layers[li]);
		lv_obj_remove_style_all(layers[li]);
		for (uint32_t k = lv_obj_get_event_count(layers[li]); k > 0; k--) {
			lv_obj_remove_event(layers[li], k - 1);
		}
	}
	for (unsigned i = 1; i < HANDLE_SLOTS; i++) {   /* экраны, созданные старой программой */
		if (s_slot[i].kind == LVBR_HK_OBJ && s_slot[i].p != blank && s_slot[i].p != old &&
				!is_sys_layer((lv_obj_t *)s_slot[i].p) &&
				lv_obj_get_parent((lv_obj_t *)s_slot[i].p) == NULL) {
			lv_obj_delete((lv_obj_t *)s_slot[i].p);
		}
	}

	/* подписки уцелевших объектов снимаются, память всех регистраций освобождается */
	lvbr_events_reset_finish();

	/* стили освобождаются после объектов, которые их использовали */
	for (unsigned i = 1; i < HANDLE_SLOTS; i++) {
		if (s_slot[i].kind == LVBR_HK_STYLE) {
			lv_style_t *st = s_slot[i].p;
			lvbr_handle_free(((int32_t)s_slot[i].gen << HANDLE_BITS) | (int32_t)i);
			lv_style_reset(st);
			lv_free(st);
		}
	}
	style_zombies_free();   /* стили, удалённые из Lisp, но ещё использовавшиеся объектами */
	lvbr_assets_reset();

	/* всё оставшееся (series, cursors, groups ...) исчезает вместе со своим объектом */
	for (unsigned i = 1; i < HANDLE_SLOTS; i++) {
		if (s_slot[i].kind != LVBR_HK_NONE) {
			s_slot[i].kind = LVBR_HK_NONE;
			s_slot[i].p = NULL;
			s_slot[i].gen = (uint16_t)(s_slot[i].gen % GEN_MASK + 1);
		}
	}

	for (unsigned i = 0; i < lvbr_font_count && i < 64; i++) {
		s_font_h[i] = lvbr_handle_new(LVBR_HK_FONT, (void *)lvbr_font_table[i].font);
	}
	dashboard_lvgl_batch(false);   /* batch, оставленный открытым старой программой, не должен задерживать рендеринг */
	dashboard_backlight_percent(100);
}

/* Публичная обёртка: запускает reset_cb в задаче LVGL. */
void lvbr_reset(void) {
	dashboard_lvgl_call(reset_cb, NULL);
}

/* Загрузка = сброс + регистрация. Порядок важен: сброс создаёт handle шрифтов, которые register() определяет как константы Lisp. */
bool lvbr_load(void) {
	lvbr_reset();            /* сначала: создаёт handle шрифтов, которые определяет register */
	return lvbr_register();
}
