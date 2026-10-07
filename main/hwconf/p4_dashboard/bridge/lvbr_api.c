/*
 * lvbr_api.c -- превращает lv_api.def / lv_consts.def / lv_fonts.def в код.
 * При добавлении функций LVGL здесь ничего править не нужно: правьте .def-файлы.
 *
 * Для каждой строки  F(RET, lv_name, ARG0, ARG1, ...)  этот файл генерирует
 *    impl_lv_name   выполняется в задаче LVGL: распаковывает аргументы, вызывает LVGL, сохраняет результат
 *    desc_lv_name   дескриптор (имя, виды аргументов, ...)
 *    ext_lv_name    расширение LispBM: lvbr_dispatch(&desc_lv_name, ...)
 */
#include "lvbr.h"

#include <string.h>

/* ---- доступ к аргументам: kind -> выражение на C (сторона задачи LVGL) */
/* Каждый A_<kind>(c, n) даёт C-значение аргумента n вызова c, готовое к передаче в
 * функцию LVGL. Мост сохранил преобразованный Lisp-аргумент в c->a[n]
 * (см. lvbr_dispatch); для handle-видов здесь уже лежит разрешённый C-указатель.
 */
#define A_I(c, n)     ((c)->a[n].i)
#define A_U(c, n)     ((c)->a[n].u)
#define A_B(c, n)     ((c)->a[n].i != 0)
#define A_S(c, n)     ((c)->a[n].s)
/* цвет: число 0xRRGGBB -> lv_color_t */
#define A_C(c, n)     lv_color_hex((c)->a[n].u)
#define A_OBJ(c, n)   ((lv_obj_t *)(c)->a[n].p)
/* то же, что A_OBJ; случай NULL (nil) уже обработан при преобразовании */
#define A_OBJN(c, n)  ((lv_obj_t *)(c)->a[n].p)
#define A_STYLE(c, n) ((lv_style_t *)(c)->a[n].p)
#define A_FONT(c, n)  ((const lv_font_t *)(c)->a[n].p)
#define A_SER(c, n)   ((c)->a[n].p)
#define A_CUR(c, n)   ((c)->a[n].p)
#define A_GRP(c, n)   ((c)->a[n].p)

/* ---- результат: kind -> сохранение в вызов */
/* Копирует возвращённую C-строку в общий буфер lvbr_strbuf: указатель, принадлежащий LVGL,
 * может измениться или исчезнуть раньше, чем задача eval превратит его в Lisp-строку. NULL -> "".
 */
char lvbr_strbuf[LVBR_STR_MAX + 1];

static inline void put_str(lvbr_call_t *c, const char *s) {
	if (!s) {
		s = "";
	}
	size_t n = strlen(s);
	if (n > LVBR_STR_MAX) {
		n = LVBR_STR_MAX;
	}
	memcpy(lvbr_strbuf, s, n);
	lvbr_strbuf[n] = '\0';
	c->r.u = (uint32_t)n;
}
/* Каждый R_<kind>(c, call) выполняет вызов LVGL и сохраняет результат в c->r
 * (строку -- в lvbr_strbuf) в виде, который lvbr_dispatch позже преобразует обратно в Lisp-значение.
 */
#define R_V(c, call)     call
#define R_I(c, call)     ((c)->r.i = (int32_t)(call))
#define R_U(c, call)     ((c)->r.u = (uint32_t)(call))
#define R_B(c, call)     ((c)->r.i = (call) ? 1 : 0)
#define R_S(c, call)     put_str((c), (call))
#define R_C(c, call)     ((c)->r.u = lv_color_to_u32(call) & 0xFFFFFFu)
/* Результат-указатель: находит или создаёт для него handle малого целого вида k.
 * NULL отображается в handle 0; если для не-NULL указателя вернулся 0, таблица заполнена;
 * тогда только что созданный объект (функция *_create) удаляется, чтобы не остаться без handle.
 */
#define R_HANDLE(k, c, call) do { \
		void *p_ = (void *)(call); \
		(c)->r.i = lvbr_handle_for(LVBR_HK_##k, p_); \
		if (p_ && !(c)->r.i) { \
			(c)->err = "lv: out of handles"; \
			if (LVBR_HK_##k == LVBR_HK_OBJ && strstr((c)->fn->name, "_create")) { \
				lv_obj_delete((lv_obj_t *)p_); \
			} \
		} \
	} while (0)
#define R_OBJ(c, call)   R_HANDLE(OBJ, c, call)
#define R_STYLE(c, call) R_HANDLE(STYLE, c, call)
#define R_FONT(c, call)  R_HANDLE(FONT, c, call)
#define R_SER(c, call)   R_HANDLE(SER, c, call)
#define R_CUR(c, call)   R_HANDLE(CUR, c, call)
#define R_GRP(c, call)   R_HANDLE(GRP, c, call)

/* Косвенность: токен вида из lv_api.def выбирает A_<kind> / R_<kind>;
 * KIND(k) даёт соответствующее значение lvbr_kind_t для дескриптора.
 */
#define A_(k, c, n)   A_##k(c, n)
#define R_(k, c, call) R_##k(c, call)
#define KIND(k)       ((uint8_t)LVBR_K_##k)

/* ---- по одному макросу на каждое число аргументов */
/* DESC для функции LVGL n с N аргументами выпускает:
 *   ext_<n>   расширение LispBM; оно лишь передаёт управление в lvbr_dispatch(), который
 *             преобразует Lisp-аргументы и запускает impl_<n> внутри задачи LVGL
 *   desc_<n>  const-дескриптор: C-имя, ext, impl, число аргументов, вид результата и
 *             виды аргументов (перечислен в lvbr_fn_table, используется при регистрации)
 * ext_<n> объявлена заранее, так как desc_<n> указывает на неё, а она использует desc_<n>.
 */
#define DESC(R, n, N, ...) \
	static lbm_value ext_##n(lbm_value *a, lbm_uint argn); \
	static const lvbr_fn_t desc_##n = { #n, ext_##n, impl_##n, N, KIND(R), { __VA_ARGS__ }, LVBR_IX_##n }; \
	static lbm_value ext_##n(lbm_value *a, lbm_uint argn) { return lvbr_dispatch(&desc_##n, a, argn); }

/* F0..F8: по одному макросу на каждое число аргументов (препроцессор не умеет циклы).
 * F<k>(R, n, a0..) определяет impl_<n>, которая выполняется в задаче LVGL: преобразует
 * каждый аргумент через A_(kind), вызывает функцию LVGL n и сохраняет результат
 * через R_(R). Затем DESC добавляет дескриптор и расширение.
 */
#define F0(R, n) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n()); } \
	DESC(R, n, 0, 0)
#define F1(R, n, a0) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0))); } \
	DESC(R, n, 1, KIND(a0))
#define F2(R, n, a0, a1) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1))); } \
	DESC(R, n, 2, KIND(a0), KIND(a1))
#define F3(R, n, a0, a1, a2) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1), A_(a2, c, 2))); } \
	DESC(R, n, 3, KIND(a0), KIND(a1), KIND(a2))
#define F4(R, n, a0, a1, a2, a3) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1), A_(a2, c, 2), A_(a3, c, 3))); } \
	DESC(R, n, 4, KIND(a0), KIND(a1), KIND(a2), KIND(a3))
#define F5(R, n, a0, a1, a2, a3, a4) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1), A_(a2, c, 2), A_(a3, c, 3), A_(a4, c, 4))); } \
	DESC(R, n, 5, KIND(a0), KIND(a1), KIND(a2), KIND(a3), KIND(a4))
#define F6(R, n, a0, a1, a2, a3, a4, a5) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1), A_(a2, c, 2), A_(a3, c, 3), A_(a4, c, 4), A_(a5, c, 5))); } \
	DESC(R, n, 6, KIND(a0), KIND(a1), KIND(a2), KIND(a3), KIND(a4), KIND(a5))
#define F7(R, n, a0, a1, a2, a3, a4, a5, a6) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1), A_(a2, c, 2), A_(a3, c, 3), A_(a4, c, 4), A_(a5, c, 5), A_(a6, c, 6))); } \
	DESC(R, n, 7, KIND(a0), KIND(a1), KIND(a2), KIND(a3), KIND(a4), KIND(a5), KIND(a6))
#define F8(R, n, a0, a1, a2, a3, a4, a5, a6, a7) \
	static void impl_##n(lvbr_call_t *c) { R_(R, c, n(A_(a0, c, 0), A_(a1, c, 1), A_(a2, c, 2), A_(a3, c, 3), A_(a4, c, 4), A_(a5, c, 5), A_(a6, c, 6), A_(a7, c, 7))); } \
	DESC(R, n, 8, KIND(a0), KIND(a1), KIND(a2), KIND(a3), KIND(a4), KIND(a5), KIND(a6), KIND(a7))

/* Нулевой проход: номер каждой функции в lvbr_fn_table (LVBR_IX_lv_name), чтобы дескриптор знал свою позицию. */
#define F(R, n, ...) LVBR_IX_##n,
enum {
#include "lv_api.def"
	LVBR_IX_COUNT
};
#undef F

/* ---- F(RET, name, args...) -> F<число аргументов> */
/* F(RET, name, args...) выбирает F<число аргументов>: NARGS считает вариативные
 * аргументы сдвигом убывающего списка чисел (ведущий 0 и ##__VA_ARGS__
 * сохраняют корректным случай без аргументов), CAT склеивает имя после раскрытия.
 */
#define NARGS_(_0, _1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define NARGS(...) NARGS_(0, ##__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define F(R, n, ...) CAT(F, NARGS(__VA_ARGS__))(R, n, ##__VA_ARGS__)

/* Первый проход: каждая строка F() раскрывается в функции impl_/desc_/ext_. */
#include "lv_api.def"

#undef F
/* Второй проход: тот же .def-файл превращается в таблицу дескрипторов. */
#define F(R, n, ...) &desc_##n,
const lvbr_fn_t *const lvbr_fn_table[] = {
#include "lv_api.def"
};
const unsigned lvbr_fn_count = sizeof(lvbr_fn_table) / sizeof(lvbr_fn_table[0]);
#undef F

/* Константы из lv_consts.def: Lisp-имя = C-имя, плюс её значение. */
#define K(n) { #n, (uint32_t)(n) },
const lvbr_const_t lvbr_const_table[] = {
#include "lv_consts.def"
};
const unsigned lvbr_const_count = sizeof(lvbr_const_table) / sizeof(lvbr_const_table[0]);
#undef K

/* Шрифты из lv_fonts.def: Lisp-имя "font-montserrat-<size>"; шрифт по умолчанию идёт первым. */
#define FONT(n) { "font-montserrat-" #n, &lv_font_montserrat_##n },
const lvbr_font_t lvbr_font_table[] = {
	{ "font-default", LV_FONT_DEFAULT },
#include "lv_fonts.def"
};
const unsigned lvbr_font_count = sizeof(lvbr_font_table) / sizeof(lvbr_font_table[0]);
#undef FONT
