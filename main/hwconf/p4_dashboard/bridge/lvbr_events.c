/*
 * lvbr_events.c -- события LVGL для Lisp.
 *
 * LVGL вызывает callback'и внутри задачи LVGL, а код Lisp выполняется в задаче eval,
 * поэтому callback не может вызвать Lisp напрямую. Вместо этого:
 *
 *   (lv-obj-add-event-cb obj 'handler LV_EVENT_CLICKED)         ; handler = имя функции Lisp
 *   (lv-obj-add-event-cb obj 'handler LV_EVENT_CLICKED user-data) ; user-data: символ, число или nil
 *   (lv-obj-remove-event-cb obj 'handler [filter])               ; отписка -> число снятых подписок
 *
 * при наступлении события ставит его в очередь, а программа забирает его оттуда:
 *
 *   (lv-event-poll)  -> nil | e          e непрозрачное значение, используйте функции lv-event-get-*
 *   (lv-event-wait seconds) -> t | timeout  ждёт события не дольше seconds (поток Lisp спит,
 *                                         а не опрашивает очередь), затем события читаются lv-event-poll
 *   (lv-event-get-gesture-dir e) -> LV_DIR_LEFT/RIGHT/TOP/BOTTOM для LV_EVENT_GESTURE (свайп)
 *   (lv-event-get-code e)  (lv-event-get-target-obj e)  (lv-event-get-current-target-obj e)
 *   (lv-event-get-user-data e)  (lv-event-get-cb e)   -> символ обработчика
 *   (lv-event-get-x e) (lv-event-get-y e)  -> точка касания (события из lv-touch-hook и событий ввода объекта)
 *
 * Общий обработчик касаний (все нажатия на экран, на каком бы объекте они ни начались):
 *   (lv-touch-hook 'handler)   -- handler получает LV_EVENT_PRESSED и LV_EVENT_RELEASED с координатами
 *   (lv-touch-hook nil)        -- выключить
 *   (lv-touch-get)  -> (pressed x y)   текущее состояние касания (pressed 1/0), nil если тачскрина нет
 *
 * Повторяющиеся события (PRESSING, VALUE_CHANGED, SCROLL...) объединяются, пока
 * не прочитаны, поэтому палец, двигающий slider, не может переполнить очередь.
 *
 * Регистрации выделяются по одной (lv_malloc) и освобождаются вместе с объектом, поэтому
 * их число ограничено только памятью. Память регистрации освобождается не сразу в
 * LV_EVENT_DELETE (в этой же рассылке trampoline ещё может получить её адрес), а в
 * lvbr_events_gc() -- из вызовов моста, которые никогда не выполняются внутри рассылки события.
 */
#include "lvbr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* Ёмкость кольцевой очереди ожидающих событий. */
#define QUEUE_LEN    128

typedef enum { UD_NIL = 0, UD_SYM, UD_INT } ud_kind_t;

/* Состояние регистрации: LIVE -- рабочая; DYING -- объект удаляется (доставляется только
 * LV_EVENT_DELETE); DEAD -- снята или забыта при сбросе (ничего не доставляется). */
typedef enum { RS_LIVE = 0, RS_DYING, RS_DEAD } rstate_t;

typedef struct reg {
	struct reg *next, *prev;   /* список s_live (двусвязный) или s_zombie (по next) */
	lv_obj_t   *obj;
	uint32_t    filter;        /* lv_event_code_t, LV_EVENT_ALL = любое событие */
	lbm_value   cb;            /* символ */
	lbm_value   ud_sym;
	int32_t     ud_int;
	uint8_t     ud_kind;
	uint8_t     state;
	uint8_t     linked;        /* 1, пока в s_live */
} reg_t;

typedef struct {
	lbm_value  cb;
	int32_t    code;
	int32_t    target;
	int32_t    current;
	ud_kind_t  ud_kind;
	lbm_value  ud_sym;
	int32_t    ud_int;
	int32_t    dir;        /* LV_EVENT_GESTURE: LV_DIR_* свайпа, иначе 0 */
	int32_t    val;        /* LV_EVENT_VALUE_CHANGED у slider / arc: значение в этот момент (вызов LVGL не нужен) */
	int32_t    x, y;       /* точка касания (для событий ввода), иначе 0 */
} ev_t;

/* Только задача LVGL: регистрации на живых объектах и отцепленные, ждущие освобождения. */
static reg_t   *s_live;
static reg_t   *s_zombie;
static uint32_t s_nlive;

/* s_q: кольцевой буфер, пуст при s_head == s_tail; один слот всегда остаётся свободным. */
static ev_t  s_q[QUEUE_LEN];
static volatile unsigned s_head, s_tail;
static volatile uint32_t s_dropped;    /* отброшено событий из-за переполнения (для lv-perf) */
/* очередь заполняется задачей LVGL и опустошается потоком Lisp: короткий spinlock вместо перехода в LVGL,
 * который ждал бы завершения текущего кадра */
static portMUX_TYPE s_q_lock = portMUX_INITIALIZER_UNLOCKED;

/* Поток Lisp, спящий в lv-event-wait (-1 = никто), и момент (lv_tick_get, мс), после которого
 * его уже нельзя будить: к этому времени он сам проснётся по таймауту и может заблокироваться
 * в другом месте. Защищено s_q_lock. */
static volatile int32_t  s_waiter = -1;
static volatile uint32_t s_waiter_deadline;

/* ----------------------------------------------------------- регистрации (задача LVGL) */

static void live_link(reg_t *r) {
	r->prev = NULL;
	r->next = s_live;
	if (s_live) {
		s_live->prev = r;
	}
	s_live = r;
	r->linked = 1;
	s_nlive++;
}

static void live_unlink(reg_t *r) {
	if (!r->linked) {
		return;
	}
	if (r->prev) {
		r->prev->next = r->next;
	} else {
		s_live = r->next;
	}
	if (r->next) {
		r->next->prev = r->prev;
	}
	r->next = r->prev = NULL;
	r->linked = 0;
	s_nlive--;
}

/* Отцепить регистрацию от списка живых и отложить освобождение до lvbr_events_gc(). */
static void zombie(reg_t *r, rstate_t st) {
	live_unlink(r);
	if (r->state == RS_LIVE || st == RS_DEAD) {
		r->state = (uint8_t)st;
	}
	r->next = s_zombie;
	s_zombie = r;
}

/* Освобождает отцепленные регистрации. Вызывается только вне рассылки событий LVGL. */
void lvbr_events_gc(void) {
	while (s_zombie) {
		reg_t *r = s_zombie;
		s_zombie = r->next;
		lv_free(r);
	}
}

/* ----------------------------------------------------------- очередь (задача LVGL) */

/* События, возникающие непрерывно; более новое заменяет непрочитанное старое с тем же cb/code/target. */
static bool mergeable(int32_t code) {
	return code == LV_EVENT_PRESSING || code == LV_EVENT_VALUE_CHANGED ||
		   code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_BEGIN ||
		   code == LV_EVENT_SCROLL_END;
}

/* Будит поток Lisp, спящий в lv-event-wait (вызывается вне spinlock: lbm_unblock берёт мьютекс LispBM). */
static void wake_waiter(void) {
	int32_t w = -1;
	uint32_t now = lv_tick_get();
	portENTER_CRITICAL(&s_q_lock);
	if (s_waiter >= 0) {
		if ((int32_t)(s_waiter_deadline - now) > 0) {
			w = s_waiter;
		}
		s_waiter = -1;
	}
	portEXIT_CRITICAL(&s_q_lock);
	if (w >= 0) {
		lbm_unblock_ctx_unboxed((lbm_cid)w, ENC_SYM_TRUE);
	}
}

/* Вызывается из задачи LVGL. Объединяет повторяющиеся события. При переполнении теряется
 * в первую очередь непрерывное событие (новое или самое старое из стоящих), и только если
 * таких нет -- самое старое дискретное (CLICKED и т.п.); каждая потеря считается в s_dropped. */
static void push(const ev_t *e) {
	portENTER_CRITICAL(&s_q_lock);
	if (mergeable(e->code)) {
		/* идём от новых записей к старым и останавливаемся на первой не объединяемой: иначе объединённое значение
		 * было бы доставлено раньше более позднего PRESSED / CLICKED и нарушило бы порядок событий */
		for (unsigned i = s_head; i != s_tail;) {
			i = (i + QUEUE_LEN - 1) % QUEUE_LEN;
			if (!mergeable(s_q[i].code)) {
				break;
			}
			if (s_q[i].cb == e->cb && s_q[i].code == e->code && s_q[i].target == e->target) {
				s_q[i] = *e;
				portEXIT_CRITICAL(&s_q_lock);
				return;
			}
		}
	}
	unsigned next = (s_head + 1) % QUEUE_LEN;
	if (next == s_tail) {
		s_dropped++;
		if (mergeable(e->code)) {
			portEXIT_CRITICAL(&s_q_lock);
			return;
		}
		/* убрать самое старое непрерывное событие, сдвинув более новые на его место */
		bool removed = false;
		for (unsigned i = s_tail; i != s_head; i = (i + 1) % QUEUE_LEN) {
			if (mergeable(s_q[i].code)) {
				unsigned j = i;
				for (;;) {
					unsigned k = (j + 1) % QUEUE_LEN;
					if (k == s_head) {
						break;
					}
					s_q[j] = s_q[k];
					j = k;
				}
				s_head = j;
				removed = true;
				break;
			}
		}
		if (!removed) {
			s_tail = (s_tail + 1) % QUEUE_LEN;
		}
		next = (s_head + 1) % QUEUE_LEN;
	}
	s_q[s_head] = *e;
	s_head = next;
	portEXIT_CRITICAL(&s_q_lock);
}

/* При LV_EVENT_ALL пересылаются только события ввода / значения / экрана, но не
 * покадровые уведомления отрисовки, layout и стиля. Их нужно запрашивать явно по имени. */
static bool user_level(int32_t code) {
	return (code >= LV_EVENT_PRESSED && code <= LV_EVENT_HOVER_LEAVE &&
				code != LV_EVENT_HIT_TEST && code != LV_EVENT_INDEV_RESET) ||
		   (code >= LV_EVENT_VALUE_CHANGED && code <= LV_EVENT_STATE_CHANGED) ||
		   (code >= LV_EVENT_SCREEN_UNLOAD_START && code <= LV_EVENT_SCREEN_UNLOADED);
}

/* Единый C callback для каждого зарегистрированного обработчика Lisp: делает snapshot события в ev_t и ставит его в очередь.
 * Всё копируется здесь, так как lv_event_t действителен только во время callback'а. */
static void trampoline(lv_event_t *e) {
	reg_t *r = lv_event_get_user_data(e);
	if (!r || r->state == RS_DEAD) {
		return;
	}
	int32_t code = (int32_t)lv_event_get_code(e);
	/* объект удаляется: из его подписок доставляется только само LV_EVENT_DELETE */
	if (r->state == RS_DYING && code != LV_EVENT_DELETE) {
		return;
	}
	if (r->obj != lv_event_get_current_target_obj(e)) {
		return;
	}
	if (r->filter == LV_EVENT_ALL && !user_level(code)) {
		return;
	}
	ev_t ev = {
		.cb = r->cb,
		.code = code,
		.ud_kind = (ud_kind_t)r->ud_kind,
		.ud_sym = r->ud_sym,
		.ud_int = r->ud_int,
		.dir = 0,
		.val = 0,
	};
	if (code == LV_EVENT_DELETE) {
		/* handle удаляемого объекта уже освобождён (или вот-вот будет): отдаём его прежнее значение только как
		 * идентификатор -- вызовы lv-* с ним вернут ошибку, новый handle для умирающего объекта не создаётся */
		ev.target = ev.current = lvbr_obj_handle_peek(r->obj);
	} else {
		ev.target = lvbr_obj_handle(lv_event_get_target_obj(e));
		ev.current = lvbr_obj_handle(lv_event_get_current_target_obj(e));
	}
	/* направление свайпа и точка касания корректны, только пока indev ещё активен */
	{
		lv_indev_t *in = lv_indev_active();
		if (in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
			if (code == LV_EVENT_GESTURE) {
				ev.dir = (int32_t)lv_indev_get_gesture_dir(in);
			}
			lv_point_t p;
			lv_indev_get_point(in, &p);
			ev.x = p.x;
			ev.y = p.y;
		}
	}
	if (code == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *t = lv_event_get_target_obj(e);
		if (t) {
#if LV_USE_SLIDER
			if (lv_obj_check_type(t, &lv_slider_class)) {
				ev.val = lv_slider_get_value(t);
			}
#endif
#if LV_USE_ARC
			if (lv_obj_check_type(t, &lv_arc_class)) {
				ev.val = lv_arc_get_value(t);
			}
#endif
		}
	}
	push(&ev);
	wake_waiter();
}

/* ------------------------------------------------ общий обработчик касаний (задача LVGL) */

static lbm_value s_touch_cb;    /* символ обработчика Lisp, 0 = выключено */

/* Событие ввода (PRESSED / RELEASED) самого тачскрина: приходит при любом нажатии, где бы оно ни началось. */
static void touch_ev(lv_event_t *e) {
	if (!s_touch_cb) {
		return;
	}
	lv_indev_t *in = lv_indev_active();
	if (!in) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(in, &p);
	ev_t ev = { .cb = s_touch_cb, .code = (int32_t)lv_event_get_code(e), .x = p.x, .y = p.y };
	push(&ev);
	wake_waiter();
}

static void touch_hook_set(lbm_value cb) {
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		if (lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) {
			continue;
		}
		lv_indev_remove_event_cb_with_user_data(i, touch_ev, NULL);
		if (cb) {
			lv_indev_add_event_cb(i, touch_ev, LV_EVENT_PRESSED, NULL);
			lv_indev_add_event_cb(i, touch_ev, LV_EVENT_RELEASED, NULL);
		}
	}
	s_touch_cb = cb;
}

/* Объект удаляется (LV_EVENT_DELETE, задача LVGL): его подписки переходят в DYING и уходят в очередь
 * на освобождение. Сами дескрипторы событий LVGL снимет сразу после этой рассылки. */
void lvbr_events_obj_deleted(lv_obj_t *obj) {
	uint32_t n = lv_obj_get_event_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
		if (d && lv_event_dsc_get_cb(d) == trampoline) {
			reg_t *r = lv_event_dsc_get_user_data(d);
			if (r && r->linked) {
				zombie(r, RS_DYING);
			}
		}
	}
}

/* Уплотняет кольцо на месте, оставляя только события, не связанные с handle h. */
void lvbr_events_forget(int32_t h) {
	/* отбрасываем события в очереди, объект которых удалён (его handle может быть использован повторно) */
	portENTER_CRITICAL(&s_q_lock);
	unsigned w = s_tail;
	for (unsigned i = s_tail; i != s_head; i = (i + 1) % QUEUE_LEN) {
		if (s_q[i].target == h || s_q[i].current == h) {
			continue;
		}
		s_q[w] = s_q[i];
		w = (w + 1) % QUEUE_LEN;
	}
	s_head = w;
	portEXIT_CRITICAL(&s_q_lock);
}

static void queue_clear(void) {
	portENTER_CRITICAL(&s_q_lock);
	s_head = s_tail = 0;
	s_waiter = -1;
	portEXIT_CRITICAL(&s_q_lock);
}

/* Начало сброса (перезагрузка Lisp-программы): все подписки перестают доставлять события, очередь очищается.
 * Память освобождает lvbr_events_reset_finish() после удаления объектов старой программы. */
void lvbr_events_reset(void) {
	touch_hook_set(0);
	for (reg_t *r = s_live; r; r = r->next) {
		r->state = RS_DEAD;
	}
	for (reg_t *r = s_zombie; r; r = r->next) {
		r->state = RS_DEAD;
	}
	queue_clear();
}

/* Конец сброса: подписки, чьи объекты пережили сброс (системные слои, экран при нехватке памяти),
 * снимаются с объектов, и вся память регистраций освобождается. */
void lvbr_events_reset_finish(void) {
	while (s_live) {
		reg_t *r = s_live;
		live_unlink(r);
		/* объект жив (иначе регистрация ушла бы в s_zombie при его удалении) */
		lv_obj_remove_event_cb_with_user_data(r->obj, trampoline, r);
		lv_free(r);
	}
	lvbr_events_gc();
	s_nlive = 0;
	queue_clear();
	s_dropped = 0;
}

uint32_t lvbr_events_dropped(void) {
	return s_dropped;
}

uint32_t lvbr_events_live(void) {
	return s_nlive;
}

/* --------------------------------------------------------------- расширения */

typedef struct {
	int32_t    obj;
	lbm_value  cb;
	uint32_t   filter;
	ud_kind_t  ud_kind;
	lbm_value  ud_sym;
	int32_t    ud_int;
	bool       any_filter;   /* remove: снять подписки с любым фильтром */
	int32_t    removed;
	const char *err;
} add_args_t;

/* Выполняется в задаче LVGL: выделяет регистрацию и подключает trampoline (адрес регистрации = user data). */
static void add_cb(void *arg) {
	add_args_t *a = arg;
	lvbr_events_gc();
	lv_obj_t *obj = lvbr_handle_get(LVBR_HK_OBJ, a->obj);
	if (!obj) {
		a->err = "lv: invalid or deleted handle";
		return;
	}
	reg_t *r = lv_malloc(sizeof(reg_t));
	if (!r) {
		a->err = "lv: out of memory for event callback";
		return;
	}
	memset(r, 0, sizeof(*r));
	r->obj = obj;
	r->filter = a->filter;
	r->cb = a->cb;
	r->ud_kind = (uint8_t)a->ud_kind;
	r->ud_sym = a->ud_sym;
	r->ud_int = a->ud_int;
	r->state = RS_LIVE;
	live_link(r);
	if (!lv_obj_add_event_cb(obj, trampoline, (lv_event_code_t)a->filter, r)) {
		live_unlink(r);
		lv_free(r);
		a->err = "lv: out of memory for event callback";
	}
}

/* Выполняется в задаче LVGL: снимает подписки объекта с этим обработчиком (и фильтром, если задан). */
static void remove_cb(void *arg) {
	add_args_t *a = arg;
	lvbr_events_gc();
	lv_obj_t *obj = lvbr_handle_get(LVBR_HK_OBJ, a->obj);
	if (!obj) {
		a->err = "lv: invalid or deleted handle";
		return;
	}
	/* с конца: снятие дескриптора сдвигает индексы следующих */
	for (uint32_t i = lv_obj_get_event_count(obj); i > 0; i--) {
		lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i - 1);
		if (!d || lv_event_dsc_get_cb(d) != trampoline) {
			continue;
		}
		reg_t *r = lv_event_dsc_get_user_data(d);
		if (!r || !r->linked || r->cb != a->cb || (!a->any_filter && r->filter != a->filter)) {
			continue;
		}
		lv_obj_remove_event_dsc(obj, d);
		zombie(r, RS_DEAD);
		a->removed++;
	}
	lvbr_events_gc();
}

static bool parse_obj_cb(lbm_value *args, add_args_t *a) {
	if (!lbm_is_number(args[0]) || !lbm_is_symbol(args[1]) || lbm_is_symbol_nil(args[1])) {
		return false;
	}
	a->obj = lbm_dec_as_i32(args[0]);
	a->cb = args[1];
	return true;
}

/* (lv-obj-add-event-cb obj 'handler filter [user-data]) */
static lbm_value ext_add_event_cb(lbm_value *args, lbm_uint argn) {
	if (argn != 3 && argn != 4) {
		lbm_set_error_reason("lv-obj-add-event-cb: (obj 'handler filter [user-data])");
		return ENC_SYM_EERROR;
	}
	add_args_t a = { 0 };
	int32_t filter;
	if (!parse_obj_cb(args, &a) || !lvbr_arg_i32(args[2], &filter)) {
		lbm_set_error_reason("lv-obj-add-event-cb: (obj 'handler filter [user-data])");
		return ENC_SYM_TERROR;
	}
	a.filter = (uint32_t)filter;
	if (argn == 4) {
		lbm_value u = args[3];
		if (lbm_is_symbol_nil(u)) {
			a.ud_kind = UD_NIL;
		} else if (lbm_is_symbol(u)) {
			a.ud_kind = UD_SYM; a.ud_sym = u;
		} else if (lbm_is_number(u)) {
			/* значение должно помещаться в lbm_enc_i (28 бит): тогда event_poll не выделяет память после извлечения из очереди */
			lbm_uint t = lbm_type_of(u);
			if (t == LBM_TYPE_FLOAT || t == LBM_TYPE_DOUBLE) {
				double d = t == LBM_TYPE_FLOAT ? (double)lbm_dec_as_float(u) : lbm_dec_as_double(u);
				if (!(d >= -2147483648.0 && d <= 2147483647.0)) {
					lbm_set_error_reason("lv-obj-add-event-cb: user-data number out of range");
					return ENC_SYM_TERROR;
				}
			}
			int32_t iv = lbm_dec_as_i32(u);
			if (lbm_dec_as_i32(lbm_enc_i(iv)) != iv) {
				lbm_set_error_reason("lv-obj-add-event-cb: user-data integer must fit in 28 bits");
				return ENC_SYM_TERROR;
			}
			a.ud_kind = UD_INT; a.ud_int = iv;
		} else {
			lbm_set_error_reason("lv-obj-add-event-cb: user-data must be a symbol, number or nil");
			return ENC_SYM_TERROR;
		}
	}
	if (!lvbr_run(add_cb, &a)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	if (a.err) {
		lbm_set_error_reason(a.err);
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

/* (lv-obj-remove-event-cb obj 'handler [filter]) -> число снятых подписок */
static lbm_value ext_remove_event_cb(lbm_value *args, lbm_uint argn) {
	add_args_t a = { 0 };
	int32_t filter = 0;
	if ((argn != 2 && argn != 3) || !parse_obj_cb(args, &a) || (argn == 3 && !lvbr_arg_i32(args[2], &filter))) {
		lbm_set_error_reason("lv-obj-remove-event-cb: (obj 'handler [filter])");
		return ENC_SYM_TERROR;
	}
	a.filter = (uint32_t)filter;
	a.any_filter = argn == 2;
	if (!lvbr_run(remove_cb, &a)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	if (a.err) {
		lbm_set_error_reason(a.err);
		return ENC_SYM_EERROR;
	}
	return lbm_enc_i(a.removed);
}

/* Результат извлечения из очереди под блокировкой. */
typedef struct { bool have; ev_t ev; } pop_t;

/* Восстанавливает значение user-data для Lisp из сохранённой формы. */
static lbm_value ud_value(ud_kind_t k, lbm_value sym, int32_t i) {
	switch (k) {
	case UD_SYM: return sym;
	case UD_INT: return lbm_enc_i(i);   /* значение проверено в ext_add_event_cb: помещается в lbm_enc_i, память не выделяется */
	default:     return ENC_SYM_NIL;
	}
}

/* Извлекает самое старое событие как список из 9 элементов (cb code target current user-data gesture-dir value x y) или nil. */
static lbm_value ext_event_poll(lbm_value *args, lbm_uint argn) {
	(void)args;
	if (argn != 0) {
		lbm_set_error_reason("lv-event-poll: no arguments");
		return ENC_SYM_EERROR;
	}
	/* очередь пуста (обычный случай на каждом тике): без перехода в задачу LVGL, который ждал бы завершения текущего кадра */
	if (s_tail == s_head) {
		return ENC_SYM_NIL;
	}
	/* сначала выделяем результат: если heap заполнен, LispBM запускает GC и вызывает эту функцию снова, а событие остаётся в очереди */
	lbm_value res = lbm_heap_allocate_list(9);
	if (lbm_is_symbol(res)) {
		return res;
	}
	pop_t p = { 0 };
	portENTER_CRITICAL(&s_q_lock);
	if (s_tail != s_head) {
		p.ev = s_q[s_tail];
		s_tail = (s_tail + 1) % QUEUE_LEN;
		p.have = true;
	}
	portEXIT_CRITICAL(&s_q_lock);
	if (!p.have) {
		return ENC_SYM_NIL;
	}
	/* 0 означает отсутствие объекта: возвращается nil, а не фиктивный handle */
	lbm_value vals[9] = {
		p.ev.cb,
		lbm_enc_i(p.ev.code),
		p.ev.target ? lbm_enc_i(p.ev.target) : ENC_SYM_NIL,
		p.ev.current ? lbm_enc_i(p.ev.current) : ENC_SYM_NIL,
		ud_value(p.ev.ud_kind, p.ev.ud_sym, p.ev.ud_int),
		lbm_enc_i(p.ev.dir),
		lbm_enc_i(p.ev.val),
		lbm_enc_i(p.ev.x),
		lbm_enc_i(p.ev.y)
	};
	lbm_value c = res;
	for (int i = 0; i < 9 && lbm_is_cons(c); i++) {
		lbm_set_car(c, vals[i]);
		c = lbm_cdr(c);
	}
	return res;
}

/* (lv-event-wait seconds) -> t, если события есть или пришли за это время; символ timeout, если нет.
 * Поток Lisp блокируется (не опрашивает очередь) и просыпается сразу при первом событии.
 * seconds <= 0: не ждать, t или nil по наличию событий. */
static lbm_value ext_event_wait(lbm_value *args, lbm_uint argn) {
	if (argn != 1 || !lbm_is_number(args[0])) {
		lbm_set_error_reason("lv-event-wait: (seconds)");
		return ENC_SYM_TERROR;
	}
	float t = lbm_dec_as_float(args[0]);
	if (s_tail != s_head) {
		return ENC_SYM_TRUE;
	}
	if (!(t > 0.0f)) {
		return ENC_SYM_NIL;
	}
	if (t > 60.0f) {
		t = 60.0f;
	}
	/* Сначала блокировка (до возврата из расширения LispBM держит свой мьютекс, и lbm_unblock_ctx из задачи LVGL
	 * дождётся, пока контекст действительно окажется в списке заблокированных: пробуждение не теряется),
	 * затем повторная проверка очереди и регистрация ожидающего под тем же spinlock, что и push(). */
	lbm_block_ctx_from_extension_timeout(t);
	uint32_t ms = (uint32_t)(t * 1000.0f);
	portENTER_CRITICAL(&s_q_lock);
	if (s_tail != s_head) {
		portEXIT_CRITICAL(&s_q_lock);
		lbm_undo_block_ctx_from_extension();
		return ENC_SYM_TRUE;
	}
	s_waiter = (int32_t)lbm_get_current_cid();
	/* запас 3 мс: около таймаута не будим -- контекст мог уже проснуться сам */
	s_waiter_deadline = lv_tick_get() + (ms > 3 ? ms - 3 : 0);
	portEXIT_CRITICAL(&s_q_lock);
	return ENC_SYM_TRUE;
}

/* Возвращает элемент ix списка события; событие — обычный список, поэтому этим геттерам вызов LVGL не нужен. */
static lbm_value event_field(lbm_value *args, lbm_uint argn, int ix) {
	if (argn != 1 || !lbm_is_cons(args[0])) {
		lbm_set_error_reason("lv-event-get-*: expects the event from (lv-event-poll)");
		return ENC_SYM_TERROR;
	}
	lbm_value l = args[0];
	for (int i = 0; i < ix; i++) {
		l = lbm_is_cons(l) ? lbm_cdr(l) : ENC_SYM_NIL;
	}
	return lbm_is_cons(l) ? lbm_car(l) : ENC_SYM_NIL;
}

static lbm_value ext_get_cb(lbm_value *a, lbm_uint n)       { return event_field(a, n, 0); }
static lbm_value ext_get_code(lbm_value *a, lbm_uint n)     { return event_field(a, n, 1); }
static lbm_value ext_get_target(lbm_value *a, lbm_uint n)   { return event_field(a, n, 2); }
static lbm_value ext_get_current(lbm_value *a, lbm_uint n)  { return event_field(a, n, 3); }
static lbm_value ext_get_user(lbm_value *a, lbm_uint n)     { return event_field(a, n, 4); }
static lbm_value ext_get_dir(lbm_value *a, lbm_uint n)      { return event_field(a, n, 5); }
static lbm_value ext_get_value(lbm_value *a, lbm_uint n)    { return event_field(a, n, 6); }
static lbm_value ext_get_x(lbm_value *a, lbm_uint n)        { return event_field(a, n, 7); }
static lbm_value ext_get_y(lbm_value *a, lbm_uint n)        { return event_field(a, n, 8); }

/* (lv-touch-hook 'handler) / (lv-touch-hook nil) */
static void touch_hook_cb(void *arg) {
	touch_hook_set(*(lbm_value *)arg);
}
static lbm_value ext_touch_hook(lbm_value *args, lbm_uint argn) {
	if (argn != 1 || !lbm_is_symbol(args[0])) {
		lbm_set_error_reason("lv-touch-hook: ('handler) or (nil)");
		return ENC_SYM_TERROR;
	}
	lbm_value cb = lbm_is_symbol_nil(args[0]) ? 0 : args[0];
	if (!lvbr_run(touch_hook_cb, &cb)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

/* (lv-touch-get) -> (pressed x y) первого тачскрина или nil */
static void touch_get_cb(void *arg) {
	int32_t *o = arg;
	o[0] = -1;
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
			lv_point_t p;
			lv_indev_get_point(i, &p);
			o[0] = lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED ? 1 : 0;
			o[1] = p.x;
			o[2] = p.y;
			return;
		}
	}
}
static lbm_value ext_touch_get(lbm_value *args, lbm_uint argn) {
	(void)args;
	if (argn != 0) {
		lbm_set_error_reason("lv-touch-get: no arguments");
		return ENC_SYM_TERROR;
	}
	/* результат выделяется до вызова: при нехватке heap LispBM соберёт мусор и вызовет функцию снова */
	lbm_value res = lbm_heap_allocate_list(3);
	if (lbm_is_symbol(res)) {
		return res;
	}
	int32_t o[3] = { -1, 0, 0 };
	if (!lvbr_run(touch_get_cb, o)) {
		lbm_set_error_reason("lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	if (o[0] < 0) {
		return ENC_SYM_NIL;
	}
	lbm_value c = res;
	for (int i = 0; i < 3 && lbm_is_cons(c); i++) {
		lbm_set_car(c, lbm_enc_i(o[i]));
		c = lbm_cdr(c);
	}
	return res;
}

/* (lv-obj-send-event obj code) -- генерирует событие так, как будто оно пришло от пользователя */
typedef struct { int32_t obj; int32_t code; const char *err; } send_t;
static void send_cb(void *arg) {
	send_t *s = arg;
	lv_obj_t *o = lvbr_handle_get(LVBR_HK_OBJ, s->obj);
	if (!o) {
		s->err = "lv: invalid or deleted handle";
		return;
	}
	lv_obj_send_event(o, (lv_event_code_t)s->code, NULL);
}
static lbm_value ext_send_event(lbm_value *args, lbm_uint argn) {
	send_t s = { 0 };
	if (argn != 2 || !lvbr_arg_i32(args[0], &s.obj) || !lvbr_arg_i32(args[1], &s.code)) {
		lbm_set_error_reason("lv-obj-send-event: (obj code)");
		return ENC_SYM_TERROR;
	}
	/* только пользовательские коды: служебные (draw, layout, DELETE ...) могут сломать LVGL */
	if (!user_level(s.code)) {
		lbm_set_error_reason("lv-obj-send-event: only user-level event codes are allowed");
		return ENC_SYM_TERROR;
	}
	if (!lvbr_run(send_cb, &s) || s.err) {
		lbm_set_error_reason(s.err ? s.err : "lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

const lvbr_ext_t lvbr_events_table[] = {
	{ "lv-obj-add-event-cb",              ext_add_event_cb },
	{ "lv-obj-remove-event-cb",           ext_remove_event_cb },
	{ "lv-obj-send-event",                ext_send_event },
	{ "lv-event-poll",                    ext_event_poll },
	{ "lv-event-wait",                    ext_event_wait },
	{ "lv-event-get-cb",                  ext_get_cb },
	{ "lv-event-get-code",                ext_get_code },
	{ "lv-event-get-target-obj",          ext_get_target },
	{ "lv-event-get-current-target-obj",  ext_get_current },
	{ "lv-event-get-user-data",           ext_get_user },
	{ "lv-event-get-gesture-dir",         ext_get_dir },
	{ "lv-event-get-value",               ext_get_value },
	{ "lv-event-get-x",                   ext_get_x },
	{ "lv-event-get-y",                   ext_get_y },
	{ "lv-touch-hook",                    ext_touch_hook },
	{ "lv-touch-get",                     ext_touch_get },
};
const unsigned lvbr_events_count = sizeof(lvbr_events_table) / sizeof(lvbr_events_table[0]);
