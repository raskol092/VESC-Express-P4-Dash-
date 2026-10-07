#ifndef LVBR_H_
#define LVBR_H_

/*
 * Мост LispBM <-> LVGL, внутренний заголовок.
 *
 * Как он устроен (чтобы что-то добавить, ядро почти не приходится трогать):
 *
 *   lv_api.def     одна строка на функцию LVGL      F(RET, lv_name, ARG...)
 *   lv_consts.def  одна строка на константу         K(LV_NAME)
 *   lvbr_extra.c   расширения, написанные вручную (цвета, события, массивы...)
 *   lvbr_api.c     раскрывает два .def-файла в функции-расширения
 *   lvbr_core.c    handle, маршалинг, очередь событий, сброс
 *
 * Lisp-имя функции LVGL = C-имя с заменой '_' -> '-'.
 * Константы сохраняют своё C-имя (LV_PART_MAIN).
 *
 * Потоки: расширения выполняются в задаче eval LispBM (крошечный стек). Они только
 * преобразуют аргументы; сам вызов LVGL выполняется в задаче LVGL через
 * dashboard_lvgl_call(). Таблица handle, очередь событий и обратные вызовы
 * затрагиваются только там.
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "lispbm.h"

/* Максимальное число аргументов функции LVGL (соответствует макросам F0..F8). */
#define LVBR_MAX_ARGS   8
#define LVBR_STR_MAX    2047     /* самая длинная строка, возвращаемая в Lisp (lv-label-get-text и т.п.) */

/* ------------------------------------------------------------------ виды (kinds) */

/* Виды handle: указатели, которые Lisp хранит как малые целые.
 * Новый вид handle = одна строка здесь (+ имя вида в lv_api.def). */
/* Список видов handle в виде X-макроса; ниже он раскрывается в enum видов handle и
 * enum видов аргументов, чтобы оба оставались синхронными.
 */
#define LVBR_HKINDS(X) \
	X(OBJ)   /* lv_obj_t *            */ \
	X(STYLE) /* lv_style_t *          */ \
	X(FONT)  /* const lv_font_t *     */ \
	X(SER)   /* lv_chart_series_t *   */ \
	X(CUR)   /* lv_chart_cursor_t *   */ \
	X(GRP)   /* lv_group_t *          */

/* Вид handle, записываемый с каждым handle, чтобы handle одного вида нельзя было
 * использовать как другой. LVBR_HK_NONE означает «не handle».
 */
typedef enum {
	LVBR_HK_NONE = 0,
#define X(n) LVBR_HK_##n,
	LVBR_HKINDS(X)
#undef X
	LVBR_HK_COUNT
} lvbr_hkind_t;

/* виды аргументов / результатов, используемые в lv_api.def */
typedef enum {
	LVBR_K_V = 0,   /* void (только для результата) */
	LVBR_K_I,       /* int32 */
	LVBR_K_U,       /* uint32 */
	LVBR_K_B,       /* bool: t / nil / число */
	LVBR_K_S,       /* строка */
	LVBR_K_C,       /* цвет 0xRRGGBB */
	LVBR_K_OBJN,    /* handle объекта или nil (NULL) */
#define X(n) LVBR_K_##n,
	LVBR_HKINDS(X)
#undef X
} lvbr_kind_t;

/* Вид handle, соответствующий виду аргумента (LVBR_HK_NONE для простых значений). */
static inline lvbr_hkind_t lvbr_kind_hkind(lvbr_kind_t k) {
	switch (k) {
	case LVBR_K_OBJN: return LVBR_HK_OBJ;
#define X(n) case LVBR_K_##n: return LVBR_HK_##n;
	LVBR_HKINDS(X)
#undef X
	default: return LVBR_HK_NONE;
	}
}

/* ------------------------------------------------------------------ вызовы */

/* Один слот аргумента или результата; активный член зависит от вида. */
typedef union {
	int32_t     i;
	uint32_t    u;
	const char *s;
	void       *p;    /* разрешённый handle (действителен только внутри задачи LVGL) */
} lvbr_val_t;

/* Опережающее объявление: lvbr_impl_t и lvbr_call_t ссылаются друг на друга. */
struct lvbr_call;
typedef void (*lvbr_impl_t)(struct lvbr_call *c);

/* Дескриптор одной функции LVGL, генерируется F() в lvbr_api.c.
 * impl выполняется в задаче LVGL; ext — расширение LispBM, вызывающее lvbr_dispatch.
 */
typedef struct {
	const char   *name;                 /* C-имя, например "lv_obj_set_pos" */
	lbm_value   (*ext)(lbm_value *, lbm_uint);
	lvbr_impl_t   impl;
	uint8_t       nargs;
	uint8_t       ret;                  /* lvbr_kind_t */
	uint8_t       arg[LVBR_MAX_ARGS];   /* lvbr_kind_t */
	uint16_t      idx;                  /* номер в lvbr_fn_table (для кешированных правил проверки) */
} lvbr_fn_t;

/* Состояние одного вызова Lisp -> LVGL, передаётся из задачи eval в задачу LVGL. */
typedef struct lvbr_call {
	const lvbr_fn_t *fn;
	lvbr_val_t a[LVBR_MAX_ARGS];
	lvbr_val_t r;                       /* для строкового результата: длина строки в lvbr_strbuf */
	const char *err;                    /* устанавливается стороной LVGL при ошибке */
} lvbr_call_t;

/* Строковый результат: задача LVGL копирует строку сюда, задача eval превращает её в строку Lisp сразу после
 * возврата вызова. Буфер статический (стек задачи eval ~3 КБ), вызовы моста строго последовательны. */
extern char lvbr_strbuf[LVBR_STR_MAX + 1];

/* Сторона задачи eval: преобразует аргументы, запускает вызов в задаче LVGL, преобразует результат. */
lbm_value lvbr_dispatch(const lvbr_fn_t *f, lbm_value *args, lbm_uint argn);

/* ---------------------------------------------------------------- handle */
/* Всё ниже: только в задаче LVGL. Handle = (generation << 12) | slot, поэтому
 * устаревший handle удалённого объекта обнаруживается, а не попадает в другой объект. */

int32_t  lvbr_handle_new(lvbr_hkind_t kind, void *ptr);   /* 0 = слоты закончились */
void    *lvbr_handle_get(lvbr_hkind_t kind, int32_t h);   /* NULL, если недействителен */
void     lvbr_handle_free(int32_t h);
int32_t  lvbr_obj_handle(lv_obj_t *obj);                  /* найти или создать, 0 для NULL */
/* handle объекта без создания нового (0, если его нет); для удаляемого объекта -- прежнее значение
 * как идентификатор (недействителен для вызовов lv-*). */
int32_t  lvbr_obj_handle_peek(lv_obj_t *obj);

/* ---------------------------------------------------------- расширения, написанные вручную */

/* Расширение, написанное вручную: его Lisp-имя и реализация для LispBM. */
typedef struct {
	const char *lisp_name;     /* литерал, например "lv-color-hex" */
	lbm_value (*ext)(lbm_value *, lbm_uint);
} lvbr_ext_t;

/* Выполнить fn(arg) в задаче LVGL и дождаться завершения. */
bool lvbr_run(void (*fn)(void *), void *arg);

/* Вспомогательные функции аргументов для расширений, написанных вручную. Каждая возвращает false и задаёт
 * причину ошибки, если аргумент неверен. */
bool lvbr_arg_i32(lbm_value v, int32_t *out);
bool lvbr_arg_str(lbm_value v, const char **out);

/* Группы расширений, написанных вручную. Новый файл = ещё одна строка в s_groups[] (lvbr_core.c). */
extern const lvbr_ext_t lvbr_extra_table[];    /* lvbr_extra.c  */
extern const unsigned   lvbr_extra_count;
extern const lvbr_ext_t lvbr_events_table[];   /* lvbr_events.c */
extern const unsigned   lvbr_events_count;
extern const lvbr_ext_t lvbr_assets_table[];   /* lvbr_assets.c: шрифты и изображения в формате VESC .bin */
extern const unsigned   lvbr_assets_count;
void lvbr_assets_reset(void);

/* Вызывается в задаче LVGL при (пере)загрузке Lisp-программы: забыть собственное состояние. */
void lvbr_extra_reset(void);
void lvbr_events_reset(void);
/* Вызывается в задаче LVGL, когда handle / объект уничтожается. */
void lvbr_events_forget(int32_t handle);
void lvbr_events_obj_deleted(lv_obj_t *obj);
/* Освобождает отцепленные регистрации событий; только вне рассылки событий LVGL (вызовы моста). */
void lvbr_events_gc(void);
/* Конец сброса: снимает оставшиеся подписки и освобождает их память (после удаления объектов). */
void lvbr_events_reset_finish(void);
uint32_t lvbr_events_dropped(void);   /* потеряно событий из-за переполнения очереди */
uint32_t lvbr_events_live(void);      /* число действующих подписок */

/* найти или создать handle для указателя любого вида (0 для NULL) */
int32_t lvbr_handle_for(lvbr_hkind_t kind, void *ptr);
/* стили, принадлежащие мосту */
int32_t lvbr_style_new(void);
bool    lvbr_style_delete(int32_t handle);

/* ------------------------------------------------------------- вход в ядро */

/* сгенерированные таблицы (lvbr_api.c) */
extern const lvbr_fn_t *const lvbr_fn_table[];
extern const unsigned         lvbr_fn_count;
typedef struct { const char *name; const lv_font_t *font; } lvbr_font_t;
extern const lvbr_font_t lvbr_font_table[];
extern const unsigned    lvbr_font_count;
typedef struct { const char *name; uint32_t value; } lvbr_const_t;
extern const lvbr_const_t lvbr_const_table[];
extern const unsigned     lvbr_const_count;

/* Регистрирует в LispBM все расширения, константы и шрифты. Безопасно вызывать повторно при перезапуске. */
bool lvbr_register(void);
extern unsigned lvbr_fail_ext, lvbr_fail_def;
/* Сбрасывает всё, что создала предыдущая Lisp-программа (объекты, стили, обратные вызовы). */
void lvbr_reset(void);
/* сброс + регистрация: единственный вызов, нужный при (пере)загрузке Lisp-программы */
bool lvbr_load(void);

#endif /* LVBR_H_ */
