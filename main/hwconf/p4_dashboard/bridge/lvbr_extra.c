/*
 * lvbr_extra.c -- расширения, написанные вручную: всё, что нельзя описать простой
 * строкой в lv_api.def (функции нужен список Lisp, объект, которым владеет bridge,
 * возможность платы и т. п.).
 *
 * Чтобы добавить функцию:  1. напишите  static lbm_value ext_xxx(lbm_value *args, lbm_uint argn)
 *                          2. добавьте одну строку в lvbr_extra_table[] в конце файла.
 *
 * Правила для тела функции (она выполняется в задаче eval LispBM, стек ~3 KB):
 *   - проверьте и преобразуйте аргументы, при ошибке верните ENC_SYM_TERROR (+ lbm_set_error_reason)
 *   - всё, что обращается к LVGL, идёт через lvbr_run(fn, arg); fn выполняется в
 *     задаче LVGL, handles разрешаются там через lvbr_handle_get().
 */
#include "lvbr.h"
#include "hw_p4_dashboard.h"

#include <string.h>

/* ------------------------------------------------------------------ цвета */

/* (lv-color-hex 0xRRGGBB) -> значение цвета. Цвета — это обычные числа 0xRRGGBB. */
static lbm_value ext_color_hex(lbm_value *args, lbm_uint argn) {
	int32_t v;
	if (argn != 1 || !lvbr_arg_i32(args[0], &v)) {
		return ENC_SYM_TERROR;
	}
	return lbm_enc_i(v & 0xFFFFFF);
}

/* (lv-color-make r g b) */
static lbm_value ext_color_make(lbm_value *args, lbm_uint argn) {
	int32_t r, g, b;
	if (argn != 3 || !lvbr_arg_i32(args[0], &r) || !lvbr_arg_i32(args[1], &g) || !lvbr_arg_i32(args[2], &b)) {
		return ENC_SYM_TERROR;
	}
	return lbm_enc_i(((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
}

/* (lv-pct 50) -> размер/позиция в процентах (передавайте туда, где LVGL ожидает lv_pct(50)) */
static lbm_value ext_pct(lbm_value *args, lbm_uint argn) {
	int32_t v;
	if (argn != 1 || !lvbr_arg_i32(args[0], &v)) {
		return ENC_SYM_TERROR;
	}
	return lbm_enc_i32(lv_pct(v));
}

/* ------------------------------------------------------------------- стили */

/* (lv-style-create) -> handle стиля;  (lv-style-delete style) */
static void style_create_cb(void *arg) {
	*(int32_t *)arg = lvbr_style_new();
}

static lbm_value ext_style_create(lbm_value *args, lbm_uint argn) {
	(void)args;
	(void)argn;
	int32_t h = 0;
	if (!lvbr_run(style_create_cb, &h) || !h) {
		lbm_set_error_reason("lv-style-create: out of memory or handles");
		return ENC_SYM_EERROR;
	}
	return lbm_enc_i(h);
}

static void style_delete_cb(void *arg) {
	int32_t *h = arg;
	*h = lvbr_style_delete(*h) ? 1 : 0;
}

static lbm_value ext_style_delete(lbm_value *args, lbm_uint argn) {
	int32_t h;
	if (argn != 1 || !lvbr_arg_i32(args[0], &h)) {
		return ENC_SYM_TERROR;
	}
	if (!lvbr_run(style_delete_cb, &h) || !h) {
		lbm_set_error_reason("lv: invalid or deleted handle");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

static void es_set_cb(void *arg);
static void touch_defaults(void);

/* Вызывается ядром в задаче LVGL при перезапуске Lisp: настройки ввода, заданные старой программой
 * (lv-edge-swipe, lv-touch-set), возвращаются к исходным. Стили и объекты обрабатывает ядро. */
void lvbr_extra_reset(void) {
	bool off = false;
	es_set_cb(&off);
	touch_defaults();
}

/* -------------------------------------------------------------------- график (chart) */

#if LV_USE_CHART
#define CHART_MAX_VALUES 256
/* список Lisp копируется в этот статический буфер (стека задачи eval не хватает на 256 значений); задача LVGL копирует его в series */
static int32_t s_chart_vals[CHART_MAX_VALUES];   /* заполняется в задаче eval, читается в задаче LVGL */

/* Выполняется в задаче LVGL: handles разрешаются здесь, а не в задаче eval. */
typedef struct { int32_t chart, ser; int32_t n; const char *err; } chart_t;

static void chart_values_cb(void *arg) {
	chart_t *c = arg;
	lv_obj_t *chart = lvbr_handle_get(LVBR_HK_OBJ, c->chart);
	void *ser = lvbr_handle_get(LVBR_HK_SER, c->ser);
	if (!chart || !ser) {
		c->err = "lv: invalid or deleted handle";
		return;
	}
	if (!lv_obj_check_type(chart, &lv_chart_class)) {
		c->err = "lv: not a chart";
		return;
	}
	/* series должна принадлежать именно этому графику */
	lv_chart_series_t *it = lv_chart_get_series_next(chart, NULL);
	while (it && it != (lv_chart_series_t *)ser) {
		it = lv_chart_get_series_next(chart, it);
	}
	if (!it) {
		c->err = "lv: series does not belong to this chart";
		return;
	}
	/* не больше, чем точек в графике (иначе LVGL пишет за пределы буфера series) */
	uint32_t pc = lv_chart_get_point_count(chart);
	size_t n = (size_t)c->n;
	if (n > pc) { n = pc; }
	/* Список ЗАМЕНЯЕТ точки series: точка i = значение i, точки за концом списка пусты.
	 * (lv_chart_set_series_values из LVGL дописывает значения по кругу, и при списке короче числа
	 * точек на графике оставался хвост прежних значений, а порядок зависел от предыдущих вызовов.) */
	lv_chart_set_all_values(chart, ser, LV_CHART_POINT_NONE);   /* заодно start_point = 0 */
	for (size_t i = 0; i < n; i++) {
		lv_chart_set_series_value_by_id(chart, ser, (uint32_t)i, s_chart_vals[i]);
	}
	lv_chart_refresh(chart);
}

/* (lv-chart-set-series-values chart series (list v0 v1 ...)) -- точки series становятся ровно этим списком */
static lbm_value ext_chart_set_series_values(lbm_value *args, lbm_uint argn) {
	chart_t c = { 0 };
	if (argn != 3 || !lvbr_arg_i32(args[0], &c.chart) || !lvbr_arg_i32(args[1], &c.ser) ||
			(!lbm_is_cons(args[2]) && !lbm_is_symbol_nil(args[2]))) {
		lbm_set_error_reason("lv-chart-set-series-values: (chart series list)");
		return ENC_SYM_TERROR;
	}
	for (lbm_value l = args[2]; lbm_is_cons(l); l = lbm_cdr(l)) {
		if (c.n >= CHART_MAX_VALUES) {
			lbm_set_error_reason("lv-chart-set-series-values: at most 256 values");
			return ENC_SYM_TERROR;
		}
		if (!lvbr_arg_i32(lbm_car(l), &s_chart_vals[c.n++])) {
			return ENC_SYM_TERROR;
		}
	}
	if (!lvbr_run(chart_values_cb, &c) || c.err) {
		lbm_set_error_reason(c.err ? c.err : "lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

#endif /* LV_USE_CHART */

/* ------------------------------------------------------------- размер дисплея */

/* Возвращает разрешение {hor, ver} дисплея по умолчанию (0, если дисплея нет). */
static void res_cb(void *arg) {
	int32_t *r = arg;
	lv_display_t *d = lv_display_get_default();
	r[0] = d ? lv_display_get_horizontal_resolution(d) : 0;
	r[1] = d ? lv_display_get_vertical_resolution(d) : 0;
}

/* (lv-hor-res) / (lv-ver-res) */
static lbm_value ext_hor_res(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;
	int32_t r[2] = { 0, 0 };
	lvbr_run(res_cb, r);
	return lbm_enc_i(r[0]);
}

static lbm_value ext_ver_res(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;
	int32_t r[2] = { 0, 0 };
	lvbr_run(res_cb, r);
	return lbm_enc_i(r[1]);
}

/* ----------------------------------------------------------------- аппаратная часть */
/* Тонкие обёртки над hw_p4_dashboard.c (сторона дисплея платы). */

/* (lv-backlight percent) 0..100, (lv-backlight-get) */
static lbm_value ext_backlight(lbm_value *args, lbm_uint argn) {
	int32_t p;
	if (argn != 1 || !lvbr_arg_i32(args[0], &p)) {
		return ENC_SYM_TERROR;
	}
	dashboard_backlight_request(p);
	return ENC_SYM_TRUE;
}

static lbm_value ext_backlight_get(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;
	return lbm_enc_i(dashboard_backlight_get());
}

/* (lv-batch 1) ... (lv-batch 0): приостановить отрисовку, пока строится экран (максимум 250 мс) */
static lbm_value ext_batch(lbm_value *args, lbm_uint argn) {
	int32_t on;
	if (argn != 1 || !lvbr_arg_i32(args[0], &on)) {
		return ENC_SYM_TERROR;
	}
	dashboard_lvgl_batch(on != 0);
	return ENC_SYM_TRUE;
}

/* (lv-edge-swipe 1) -- swipe gestures at the edge of a scroll container.
 * LVGL never sends LV_EVENT_GESTURE while a scroll object is active, and a container that has more snap points on an
 * axis captures every drag on that axis, even one that cannot move it (the first page dragged right, the top row
 * dragged down). Here such a drag is recognised on release: the scroll object did not move since the press and the
 * finger travelled more than the gesture distance -> LV_EVENT_GESTURE goes to the gesture target (the first ancestor
 * of the pressed object without GESTURE_BUBBLE), with the direction set in the indev like LVGL does it. */
#include "src/indev/lv_indev_private.h"
/* Цепочка объектов от нажатого вверх, запомненная при нажатии. Указатели здесь только сравниваются:
 * к моменту отпускания любой из этих объектов мог быть удалён, поэтому разыменовываются лишь объекты
 * текущей живой цепочки (активный объект indev и его родители), совпавшие с запомненными. */
typedef struct { lv_obj_t *o; int32_t sx, sy; } es_t;
static es_t s_es[8];
static int s_es_n;
static lv_point_t s_es_p0;
static bool s_es_on;
static void es_cb(lv_event_t *e) {
	lv_event_code_t c = lv_event_get_code(e);
	lv_indev_t *in = lv_indev_active();
	if (!in) {
		return;
	}
	if (c == LV_EVENT_PRESSED) {
		lv_indev_get_point(in, &s_es_p0);
		s_es_n = 0;
		for (lv_obj_t *o = lv_indev_get_active_obj(); o && s_es_n < 8; o = lv_obj_get_parent(o)) {
			s_es[s_es_n++] = (es_t){ o, lv_obj_get_scroll_x(o), lv_obj_get_scroll_y(o) };
		}
	} else if (c == LV_EVENT_RELEASED) {
		/* активный объект indev и объект прокрутки живы (LVGL обнуляет их при удалении объекта); запомненные
		 * указатели только сравниваются с ними */
		lv_obj_t *act = lv_indev_get_active_obj();
		if (!act || s_es_n == 0) {
			s_es_n = 0;
			return;
		}
		lv_obj_t *so = lv_indev_get_scroll_obj(in);
		int k = -1;
		for (int i = 0; so && i < s_es_n; i++) {
			if (s_es[i].o == so) { k = i; }
		}
		if (k < 0 || lv_obj_get_scroll_x(so) != s_es[k].sx || lv_obj_get_scroll_y(so) != s_es[k].sy) {
			s_es_n = 0;
			return;
		}
		lv_point_t p;
		lv_indev_get_point(in, &p);
		int32_t dx = p.x - s_es_p0.x, dy = p.y - s_es_p0.y;
		int32_t lim = in->gesture_min_distance;
		if (LV_ABS(dx) <= lim && LV_ABS(dy) <= lim) {
			s_es_n = 0;
			return;
		}
		/* цель жеста -- от текущего активного объекта (жив), а не от запомненного при нажатии */
		lv_obj_t *g = act;
		while (g && lv_obj_has_flag(g, LV_OBJ_FLAG_GESTURE_BUBBLE)) {
			g = lv_obj_get_parent(g);
		}
		s_es_n = 0;
		if (!g) {
			return;
		}
		in->pointer.gesture_dir = LV_ABS(dx) > LV_ABS(dy) ? (dx > 0 ? LV_DIR_RIGHT : LV_DIR_LEFT) : (dy > 0 ? LV_DIR_BOTTOM : LV_DIR_TOP);
		in->pointer.gesture_sent = 1;
		lv_obj_send_event(g, LV_EVENT_GESTURE, in);
	}
}
static void es_set_cb(void *arg) {
	bool on = *(bool *)arg;
	s_es_n = 0;
	if (on == s_es_on) {
		return;
	}
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		if (lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) {
			continue;
		}
		if (on) {
			lv_indev_add_event_cb(i, es_cb, LV_EVENT_PRESSED, NULL);
			lv_indev_add_event_cb(i, es_cb, LV_EVENT_RELEASED, NULL);
		} else {
			lv_indev_remove_event_cb_with_user_data(i, es_cb, NULL);
		}
	}
	s_es_on = on;
}
static lbm_value ext_edge_swipe(lbm_value *args, lbm_uint argn) {
	int32_t on;
	if (argn != 1 || !lvbr_arg_i32(args[0], &on)) {
		lbm_set_error_reason("lv-edge-swipe: (on)");
		return ENC_SYM_TERROR;
	}
	bool b = on != 0;
	if (!lvbr_run(es_set_cb, &b)) {
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

/* (lv-touch-set long-ms repeat-ms gesture-dist gesture-vel scroll-limit) -- настройки удержания и жестов тачскрина LVGL.
 * Значение -1 оставляет параметр как есть. long-ms: время до LONG_PRESSED, repeat-ms: период LONG_PRESSED_REPEAT,
 * gesture-dist: минимальный путь свайпа в пикселях, gesture-vel: минимальная скорость, scroll-limit: порог начала прокрутки. */
typedef struct { int32_t v[5]; } touch_t;
static int32_t touch_clamp(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : (x > hi ? hi : x); }
static void touch_cb(void *arg) {
	const touch_t *t = arg;
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		if (t->v[0] >= 0) { lv_indev_set_long_press_time(i, (uint16_t)touch_clamp(t->v[0], 50, 10000)); }
		if (t->v[1] >= 0) { lv_indev_set_long_press_repeat_time(i, (uint16_t)touch_clamp(t->v[1], 10, 5000)); }
		if (t->v[2] >= 0) { lv_indev_set_gesture_min_distance(i, (uint8_t)touch_clamp(t->v[2], 1, 255)); }
		if (t->v[3] >= 0) { lv_indev_set_gesture_min_velocity(i, (uint8_t)touch_clamp(t->v[3], 1, 255)); }
		if (t->v[4] >= 0) { lv_indev_set_scroll_limit(i, (uint8_t)touch_clamp(t->v[4], 1, 255)); }
	}
}
/* Значения LVGL по умолчанию (lv_indev.c: LV_INDEV_DEF_*), восстанавливаются при перезапуске Lisp. */
static void touch_defaults(void) {
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		lv_indev_set_long_press_time(i, 400);
		lv_indev_set_long_press_repeat_time(i, 100);
		lv_indev_set_gesture_min_distance(i, 50);
		lv_indev_set_gesture_min_velocity(i, 3);
		lv_indev_set_scroll_limit(i, 10);
	}
}

static lbm_value ext_touch_set(lbm_value *args, lbm_uint argn) {
	touch_t t;
	if (argn != 5) {
		lbm_set_error_reason("lv-touch-set: (long-ms repeat-ms gesture-dist gesture-vel scroll-limit)");
		return ENC_SYM_TERROR;
	}
	for (int k = 0; k < 5; k++) {
		if (!lvbr_arg_i32(args[k], &t.v[k])) { return ENC_SYM_TERROR; }
	}
	if (!lvbr_run(touch_cb, &t)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

/* (lv-perf) -> список из 13 элементов, статистика с момента предыдущего вызова (чтение сбрасывает её):
 *   (frames avg-us max-us rotate-us wait-us kilopixels ppa-status bufs-internal internal-free-KB chunks flush-us
 *    events-dropped callbacks)   events-dropped -- всего потеряно событий (переполнение очереди), callbacks -- подписок сейчас
 * rotate/wait/kpx/flush — суммы, а не значения на кадр; bufs-internal равно 1, когда draw buffers находятся во внутренней RAM */
/* число потерянных событий и действующих подписок (счётчик подписок -- только в задаче LVGL) */
static void perf_ev_cb(void *arg) {
	uint32_t *o = arg;
	o[0] = lvbr_events_dropped();
	o[1] = lvbr_events_live();
}

static lbm_value ext_perf(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;
	/* сначала выделяем результат: при нехватке heap LispBM запустит GC и вызовет функцию снова, а счётчики (чтение сбрасывает их) останутся нетронутыми */
	lbm_value res = lbm_heap_allocate_list(13);
	if (lbm_is_symbol(res)) {
		return res;
	}
	uint32_t f, avg, max, rot, wait, kpx;
	dashboard_lvgl_perf(&f, &avg, &max, &rot, &wait, &kpx);
	uint32_t chunks, flush_us;
	dashboard_lvgl_perf2(&chunks, &flush_us);
	uint32_t evd = 0, evl = 0;
	{
		uint32_t tmp[2] = { 0, 0 };
		lvbr_run(perf_ev_cb, tmp);
		evd = tmp[0];
		evl = tmp[1];
	}
	lbm_value vals[13] = { lbm_enc_i((int32_t)f), lbm_enc_i((int32_t)avg), lbm_enc_i((int32_t)max),
			lbm_enc_i((int32_t)rot), lbm_enc_i((int32_t)wait), lbm_enc_i((int32_t)kpx), lbm_enc_i(dashboard_lvgl_ppa_status()),
			lbm_enc_i(dashboard_lvgl_bufs_internal() ? 1 : 0), lbm_enc_i((int32_t)(dashboard_lvgl_internal_free() / 1024)),
			lbm_enc_i((int32_t)chunks), lbm_enc_i((int32_t)flush_us),
			lbm_enc_i((int32_t)(evd > 100000000u ? 100000000u : evd)), lbm_enc_i((int32_t)evl) };
	lbm_value c = res;
	for (int i = 0; i < 13 && lbm_is_cons(c); i++) {
		lbm_set_car(c, vals[i]);
		c = lbm_cdr(c);
	}
	return res;
}


/* (lv-mem) -> (internal-free-KB psram-free-KB): позволяет проекту на Lisp решить, сколько экранов можно держать построенными */
static lbm_value ext_mem(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;
	return lbm_heap_allocate_list_init(2, lbm_enc_i((int32_t)(dashboard_lvgl_internal_free() / 1024)),
			lbm_enc_i((int32_t)(dashboard_lvgl_psram_free() / 1024)));
}

/* ------------------------------------------------------------ пакеты узлов (node batches) */
/* (lv-nodes parent nodes fonts) -> список handles объектов, по одному на узел.
 * Строит целый фрагмент экрана ОДНИМ вызовом (один переход в задачу LVGL)
 * вместо ~15 вызовов на виджет, из-за которых смена экранов была медленной.
 *   nodes : список (kind pidx x y w h flags text lm style)
 *     kind  0 panel (lv_obj), 1 label
 *     pidx  0 = аргумент parent, n = n-й узел этого списка
 *     flags бит 1 не clickable, бит 2 не scrollable, бит 4 clickable
 *     text  строка (label), lm = long mode 0..4 или -1 (оставить значение LVGL по умолчанию)
 *     style плоский список (key value key value ...), только main part:
 *       1 bg 2 bg-opa 3 grad-color 4 grad-dir(1 ver) 5 radius 6 border-color
 *       7 border-width 8 shadow-color 9 shadow-width 10 text-color
 *       11 font (индекс в fonts) 12 text-align(0 l 1 c 2 r 3 auto)
 *       13 line-width 14 line-color 15 arc-width 16 arc-color 17 pad(all) 18 opa
 *   fonts : список handles шрифтов, индексируемый ключом стиля 11 */
/* лимиты одного вызова lv-nodes; таблицы node/font/output статические (слишком велики для стека задачи eval) */
#define NB_MAX   160
#define NB_STY   24
#define NB_FONTS 40

typedef struct {
	int16_t kind, pidx, lm, nst;
	int32_t x, y, w, h, fl;
	const char *text;
	int32_t st[NB_STY];
} nb_node_t;

/* заполняется в задаче eval, затем используется nb_cb в задаче LVGL (вызывающий блокируется в lvbr_run, поэтому гонки нет) */
static nb_node_t s_nb[NB_MAX];
static int32_t   s_nb_fonts[NB_FONTS];
static int32_t   s_nb_out[NB_MAX];

typedef struct { int32_t parent; int n; int nfonts; const char *err; } nb_t;

/* Применяет пары стиля (key value ...) одного узла к main part. Установка цвета также принудительно задаёт opacity 255,
 * поэтому Lisp не нужен отдельный ключ opa. Ключ 11 индексирует список fonts (с проверкой границ). */
static void nb_style(lv_obj_t *o, const nb_node_t *nd, const nb_t *a) {
	for (int i = 0; i + 1 < nd->nst; i += 2) {
		int32_t k = nd->st[i], v = nd->st[i + 1];
		switch (k) {
		case 1:  lv_obj_set_style_bg_color(o, lv_color_hex((uint32_t)v), 0); lv_obj_set_style_bg_opa(o, 255, 0); break;
		case 2:  lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); break;
		case 3:  lv_obj_set_style_bg_grad_color(o, lv_color_hex((uint32_t)v), 0); break;
		case 4:  lv_obj_set_style_bg_grad_dir(o, v == 1 ? LV_GRAD_DIR_VER : LV_GRAD_DIR_HOR, 0); break;
		case 5:  lv_obj_set_style_radius(o, v, 0); break;
		case 6:  lv_obj_set_style_border_color(o, lv_color_hex((uint32_t)v), 0); lv_obj_set_style_border_opa(o, 255, 0); break;
		case 7:  lv_obj_set_style_border_width(o, v, 0); break;
		case 8:  lv_obj_set_style_shadow_color(o, lv_color_hex((uint32_t)v), 0); lv_obj_set_style_shadow_opa(o, 255, 0); break;
		case 9:  lv_obj_set_style_shadow_width(o, v, 0); break;
		case 10: lv_obj_set_style_text_color(o, lv_color_hex((uint32_t)v), 0); lv_obj_set_style_text_opa(o, 255, 0); break;
		case 11: if (v >= 0 && v < a->nfonts) {
				const lv_font_t *f = lvbr_handle_get(LVBR_HK_FONT, s_nb_fonts[v]);
				if (f) { lv_obj_set_style_text_font(o, f, 0); }
			} break;
		case 12: lv_obj_set_style_text_align(o, v == 0 ? LV_TEXT_ALIGN_LEFT : v == 1 ? LV_TEXT_ALIGN_CENTER : v == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_AUTO, 0); break;
		case 13: lv_obj_set_style_line_width(o, v, 0); lv_obj_set_style_line_rounded(o, true, 0); break;
		case 14: lv_obj_set_style_line_color(o, lv_color_hex((uint32_t)v), 0); break;
		case 15: lv_obj_set_style_arc_width(o, v, 0); break;
		case 16: lv_obj_set_style_arc_color(o, lv_color_hex((uint32_t)v), 0); lv_obj_set_style_arc_opa(o, 255, 0); break;
		case 17: lv_obj_set_style_pad_left(o, v, 0); lv_obj_set_style_pad_right(o, v, 0);
				 lv_obj_set_style_pad_top(o, v, 0); lv_obj_set_style_pad_bottom(o, v, 0); break;
		case 18: lv_obj_set_style_opa(o, (lv_opa_t)v, 0); break;
		default: break;
		}
	}
}

/* Выполняется в задаче LVGL: создаёт все узлы по порядку. Родитель узла должен стоять в списке раньше (pidx <= i),
 * иначе используется аргумент parent. Handles объектов записываются в s_nb_out. */
/* Неудача посреди пакета: удаляем уже созданные узлы верхнего уровня (их потомки удаляются вместе с ними),
 * чтобы на экране не осталось недостроенного фрагмента. */
static void nb_rollback(lv_obj_t **objs, int n, lv_obj_t *parent) {
	for (int j = n - 1; j >= 0; j--) {
		if (objs[j] && lv_obj_get_parent(objs[j]) == parent) {
			lv_obj_delete(objs[j]);
		}
	}
}

static void nb_cb(void *arg) {
	nb_t *a = arg;
	static lv_obj_t *objs[NB_MAX];
	lv_obj_t *parent = lvbr_handle_get(LVBR_HK_OBJ, a->parent);
	if (!parent) {
		a->err = "lv-nodes: invalid or deleted parent";
		return;
	}
	for (int i = 0; i < a->n; i++) {
		const nb_node_t *nd = &s_nb[i];
		lv_obj_t *p = nd->pidx <= 0 ? parent : (nd->pidx <= i ? objs[nd->pidx - 1] : parent);
		lv_obj_t *o = nd->kind == 1 ? lv_label_create(p) : lv_obj_create(p);
		if (!o) {
			nb_rollback(objs, i, parent);
			a->err = "lv-nodes: out of memory";
			return;
		}
		objs[i] = o;
		lv_obj_set_pos(o, nd->x, nd->y);
		lv_obj_set_size(o, nd->w, nd->h);
		/* flags: 1 не clickable, 2 не scrollable, 4 clickable */
		if (nd->fl & 1) { lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE); }
		if (nd->fl & 4) { lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE); }
		if (nd->fl & 2) { lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE); }
		if (nd->kind == 1) {
			if (nd->text) { lv_label_set_text(o, nd->text); }
			if (nd->lm >= 0) {
				static const lv_label_long_mode_t modes[5] = { LV_LABEL_LONG_MODE_WRAP, LV_LABEL_LONG_MODE_DOTS,
					LV_LABEL_LONG_MODE_SCROLL, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR, LV_LABEL_LONG_MODE_CLIP };
				if (nd->lm < 5) { lv_label_set_long_mode(o, modes[nd->lm]); }
			}
		}
		nb_style(o, nd, a);
		s_nb_out[i] = lvbr_obj_handle(o);
		if (!s_nb_out[i]) {
			/* таблица handle заполнена: объект без handle Lisp не смог бы ни изменить, ни удалить */
			nb_rollback(objs, i + 1, parent);
			a->err = "lv: out of handles";
			return;
		}
	}
}

/* строгая проверка целого числа (lvbr_arg_i32 принял бы и другие типы) */
static bool nb_int(lbm_value v, int32_t *out) {
	if (!lbm_is_number(v)) { return false; }
	lbm_uint t = lbm_type_of(v);
	if (t == LBM_TYPE_FLOAT || t == LBM_TYPE_DOUBLE) {
		/* NaN и значения вне диапазона int32 отклоняются (сравнение с NaN всегда ложно) */
		double d = t == LBM_TYPE_FLOAT ? (double)lbm_dec_as_float(v) : lbm_dec_as_double(v);
		if (!(d >= -2147483648.0 && d <= 2147483647.0)) { return false; }
	} else if (t == LBM_TYPE_U64) {
		if (lbm_dec_as_u64(v) > 2147483647u) { return false; }
	} else if (t == LBM_TYPE_U32) {
		if (lbm_dec_as_u32(v) > 2147483647u) { return false; }
	} else if (t == LBM_TYPE_I64) {
		int64_t x = lbm_dec_as_i64(v);
		if (x < -2147483648LL || x > 2147483647LL) { return false; }
	}
	*out = lbm_dec_as_i32(v);
	return true;
}

/* Разбирает список узлов Lisp в s_nb (вся проверка выполняется до создания чего-либо, поэтому неверная запись ничего не строит),
 * один раз запускает nb_cb, затем возвращает handles списком. Запись: (kind pidx x y w h flags text lm [styles]);
 * слот text может быть строкой или nil. */
static lbm_value ext_nodes(lbm_value *args, lbm_uint argn) {
	nb_t a = { 0 };
	if (argn != 3 || !lvbr_arg_i32(args[0], &a.parent)) {
		lbm_set_error_reason("lv-nodes: (parent nodes fonts)");
		return ENC_SYM_TERROR;
	}
	/* и fonts, и nodes обязаны быть списками (nil допустим); иное значение — ошибка типа, а не молчаливый nil */
	lbm_value l = args[2];
	for (; lbm_is_cons(l); l = lbm_cdr(l)) {
		if (a.nfonts >= NB_FONTS || !nb_int(lbm_car(l), &s_nb_fonts[a.nfonts])) {
			lbm_set_error_reason("lv-nodes: bad font list");
			return ENC_SYM_TERROR;
		}
		a.nfonts++;
	}
	if (!lbm_is_symbol_nil(l)) {
		lbm_set_error_reason("lv-nodes: fonts must be a list");
		return ENC_SYM_TERROR;
	}
	for (l = args[1]; lbm_is_cons(l); l = lbm_cdr(l)) {
		if (a.n >= NB_MAX) {
			lbm_set_error_reason("lv-nodes: too many nodes in one call (max 160)");
			return ENC_SYM_TERROR;
		}
		nb_node_t *nd = &s_nb[a.n];
		memset(nd, 0, sizeof(*nd));
		lbm_value r = lbm_car(l);
		/* 9 полей целочисленного типа; слот 7 (text) может быть массивом или nil, и тогда он сохраняется в nd->text */
		int32_t v[9];
		for (int i = 0; i < 9; i++) {
			if (!lbm_is_cons(r)) {
				lbm_set_error_reason("lv-nodes: bad node record");
				return ENC_SYM_TERROR;
			}
			lbm_value f = lbm_car(r);
			if (i == 7 && lbm_is_array_r(f)) {
				/* текст должен быть завершён нулём внутри массива, иначе lv_label_set_text читал бы за его концом */
				lbm_array_header_t *h = lbm_dec_array_r(f);
				if (!h || !h->data || !h->size || !memchr(h->data, 0, h->size)) {
					lbm_set_error_reason("lv-nodes: text must be a NUL-terminated string");
					return ENC_SYM_TERROR;
				}
				nd->text = (const char *)h->data;
				v[7] = 0;
			} else if (i == 7 && lbm_is_symbol_nil(f)) {
				nd->text = NULL; v[7] = 0;
			} else if (!nb_int(f, &v[i])) {
				lbm_set_error_reason("lv-nodes: bad node record");
				return ENC_SYM_TERROR;
			}
			r = lbm_cdr(r);
		}
		/* диапазоны проверяются до сужения до int16: kind 0..1, pidx 0..NB_MAX, lm -1..4 */
		if (v[0] < 0 || v[0] > 1 || v[1] < 0 || v[1] > NB_MAX || v[8] < -1 || v[8] > 4) {
			lbm_set_error_reason("lv-nodes: kind/pidx/long-mode out of range");
			return ENC_SYM_TERROR;
		}
		nd->kind = (int16_t)v[0]; nd->pidx = (int16_t)v[1];
		nd->x = v[2]; nd->y = v[3]; nd->w = v[4]; nd->h = v[5]; nd->fl = v[6];
		nd->lm = (int16_t)v[8];
		if (lbm_is_cons(r)) {
			for (lbm_value s = lbm_car(r); lbm_is_cons(s) && nd->nst < NB_STY; s = lbm_cdr(s)) {
				if (!nb_int(lbm_car(s), &nd->st[nd->nst])) {
					lbm_set_error_reason("lv-nodes: bad style list");
					return ENC_SYM_TERROR;
				}
				nd->nst++;
			}
		}
		a.n++;
	}
	if (!lbm_is_symbol_nil(l)) {
		lbm_set_error_reason("lv-nodes: nodes must be a list");
		return ENC_SYM_TERROR;
	}
	if (a.n == 0) {
		return ENC_SYM_NIL;
	}
	/* результат выделяется ДО создания виджетов: при нехватке heap LispBM запускает GC и вызывает функцию снова, а экран ещё не изменён */
	lbm_value res = lbm_heap_allocate_list((lbm_uint)a.n);
	if (lbm_is_symbol(res)) {
		return res;
	}
	if (!lvbr_run(nb_cb, &a)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	if (a.err) {
		lbm_set_error_reason(a.err);
		return ENC_SYM_EERROR;
	}
	lbm_value c = res;
	for (int i = 0; i < a.n && lbm_is_cons(c); i++) {
		lbm_set_car(c, lbm_enc_i(s_nb_out[i]));
		c = lbm_cdr(c);
	}
	return res;
}

/* ------------------------------------------------------------------- таблица */

const lvbr_ext_t lvbr_extra_table[] = {
	{ "lv-color-hex",                  ext_color_hex },
	{ "lv-color-make",                 ext_color_make },
	{ "lv-pct",                        ext_pct },
	{ "lv-style-create",               ext_style_create },
	{ "lv-style-delete",               ext_style_delete },
#if LV_USE_CHART
	{ "lv-chart-set-series-values",    ext_chart_set_series_values },
#endif
	{ "lv-hor-res",                    ext_hor_res },
	{ "lv-ver-res",                    ext_ver_res },
	{ "lv-backlight",                  ext_backlight },
	{ "lv-backlight-get",              ext_backlight_get },
	{ "lv-batch",                      ext_batch },
	{ "lv-nodes",                      ext_nodes },
	{ "lv-perf",                       ext_perf },
	{ "lv-mem",                        ext_mem },
	{ "lv-touch-set",                    ext_touch_set },
	{ "lv-edge-swipe",                   ext_edge_swipe },
};
const unsigned lvbr_extra_count = sizeof(lvbr_extra_table) / sizeof(lvbr_extra_table[0]);
