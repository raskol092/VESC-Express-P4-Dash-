/*
 * Шрифты и изображения в формате VESC .bin (файлы, сделанные для старого Lisp UI:
 * font/barlow-bold-24-4c.bin, assets/icon-wifi_32.bin ...), пригодные для использования в LVGL.
 *
 *   (import "font/barlow-bold-24-4c.bin" 'fb24)
 *   (def f24 (lv-font-load fb24))                       ; -> handle шрифта (FONT)
 *   (lv-obj-set-style-text-font label f24 LV_PART_MAIN)
 *
 *   (import "assets/icon-wifi_32.bin" 'ic-wifi)
 *   (def img (lv-image-create scr))
 *   (lv-image-set-vesc img ic-wifi (list nil 0x555555 0xAAAAAA 0xFFFFFF))   ; цвет для каждого индекса палитры
 *   (lv-image-set-colors img (list nil 0x00FF00 0x00AA00 0xFFFFFF))         ; перекраска позже
 *   (lv-vesc-image-size ic-wifi)                                            ; -> (w h)
 *
 * Данные копируются, поэтому Lisp-массив можно освободить. Шрифты живут до перезапуска
 * Lisp-программы; изображения живут столько же, сколько объект изображения.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lvbr.h"
#include "esp_heap_caps.h"
#include "src/misc/cache/instance/lv_image_cache.h"

/* ------------------------------------------------------------------ вспомогательное */

/* Чтение в big-endian: файлы VESC хранят все многобайтовые значения в big endian. */
static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* VESC "float32_auto" (buffer_get_float32_auto) */
/* Декодирует собственную упаковку float32 в VESC: 23-битная мантисса, 8-битная экспонента, бит знака; значение 0 кодируется всеми нулями. */
static float f32_auto(uint32_t r) {
	int e = (int)((r >> 23) & 0xFF);
	float v = 0.0f;
	if (r & 0x7FFFFFFFu) {
		float sig = (float)(r & 0x7FFFFF) / (8388608.0f * 2.0f) + 0.5f;
		v = ldexpf(sig, e - 126);
	}
	return (r & (1u << 31)) ? -v : v;
}

/* Округление половины от нуля (round half away from zero). */
static int rnd(float v) {
	return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* ------------------------------------------------------------------- шрифты */

/* Один glyph шрифта VESC. Пиксельные данные остаются внутри скопированного файла (data + off); a8 -- лениво создаваемая кешированная распакованная копия. */
typedef struct {
	uint32_t code;
	uint32_t off;        /* смещение пикселей в vfont_t.data */
	float    adv;
	int16_t  ofs_x;
	int16_t  top;        /* верх glyph относительно базовой линии, ось y вниз */
	uint16_t w, h;
	uint8_t *a8;         /* glyph, развёрнутый в A8 (PSRAM); создаётся при первом использовании и сохраняется: распаковка при каждой отрисовке была очень медленной */
} vglyph_t;

typedef struct { uint32_t code; uint32_t off; uint32_t n; } vkern_t;

/* Загруженный шрифт. Шрифты связаны в список s_fonts и живут до lvbr_assets_reset(). */
typedef struct vfont {
	lv_font_t     font;       /* должен оставаться первым */
	struct vfont *next;
	uint8_t      *data;
	uint32_t      len;
	uint32_t      nglyphs, nkern;
	vglyph_t     *glyphs;     /* отсортировано по коду */
	vkern_t      *kern;       /* отсортировано по коду */
	uint8_t       bpp;
} vfont_t;

/* Голова списка всех загруженных шрифтов. */
static vfont_t *s_fonts;

/* Компараторы для qsort, чтобы find_glyph()/kern_x() могли выполнять бинарный поиск. */
static int cmp_glyph(const void *a, const void *b) {
	uint32_t x = ((const vglyph_t *)a)->code, y = ((const vglyph_t *)b)->code;
	return x < y ? -1 : x > y;
}
static int cmp_kern(const void *a, const void *b) {
	uint32_t x = ((const vkern_t *)a)->code, y = ((const vkern_t *)b)->code;
	return x < y ? -1 : x > y;
}

/* Бинарный поиск glyph по code point. */
static const vglyph_t *find_glyph(const vfont_t *f, uint32_t code) {
	uint32_t lo = 0, hi = f->nglyphs;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		uint32_t c = f->glyphs[mid].code;
		if (c == code) {
			return &f->glyphs[mid];
		}
		if (c < code) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

/* Поиск кернинга: бинарный поиск левого символа, затем перебор его пар (правый символ, float); каждая запись занимает 12 байт. */
static float kern_x(const vfont_t *f, uint32_t left, uint32_t right) {
	uint32_t lo = 0, hi = f->nkern;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (f->kern[mid].code < left) lo = mid + 1; else hi = mid;
	}
	if (lo >= f->nkern || f->kern[lo].code != left) {
		return 0.0f;
	}
	const uint8_t *e = f->data + f->kern[lo].off;
	for (uint32_t i = 0; i < f->kern[lo].n; i++, e += 12) {
		if (be32(e) == right) {
			return f32_auto(be32(e + 4));
		}
	}
	return 0.0f;
}

/* Callback LVGL: описывает glyph (продвижение с учётом кернинга, рамка, смещения). Возврат false означает отсутствие glyph. */
static bool font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d, uint32_t letter, uint32_t next) {
	const vfont_t *f = (const vfont_t *)font;
	/* LVGL уже обнуляет dsc, но на всякий случай обнуляем сами: все поля (outline_stroke_width, entry, req_raw_bitmap...) должны быть определены. */
	lv_memzero(d, sizeof(*d));
	/* У пробела нулевой ширины и переводов строк нет glyph. */
	if (letter == 0x200B || letter == '\n' || letter == '\r') {
		return false;
	}
	const vglyph_t *g = find_glyph(f, letter);
	if (!g) {
		return false;                 /* LVGL переключается на запасной шрифт / ничего не рисует */
	}
	float adv = g->adv;
	if (next && f->nkern) {
		adv += kern_x(f, letter, next);
	}
	int a = rnd(adv);
	d->adv_w = (uint16_t)(a < 0 ? 0 : a);
	d->box_w = g->w;
	d->box_h = g->h;
	d->ofs_x = g->ofs_x;
	d->ofs_y = (int16_t)(-(g->top + (int)g->h));   /* LVGL: низ рамки относительно базовой линии */
	d->stride = g->w;   /* кешированный A8 bitmap упакован плотно */
	d->format = LV_FONT_GLYPH_FORMAT_A8;
	d->is_placeholder = 0;
	/* Запоминаем позицию glyph (+1, 0 = нет), чтобы font_get_bitmap() не искал её заново. */
	d->gid.index = (uint32_t)(g - f->glyphs) + 1;
	return true;
}

/* Один раз распаковывает glyph в A8 bitmap в PSRAM (потоки отрисовки могут запросить один и тот же glyph одновременно:
 * побеждает первая завершённая копия). Раньше каждый glyph распаковывался попиксельно, с делением, при КАЖДОЙ отрисовке
 * каждого кадра; из-за больших цифровых шрифтов это занимало большую часть времени рендера. */
static const uint8_t *glyph_a8(const vfont_t *f, vglyph_t *g) {
	uint8_t *c = __atomic_load_n(&g->a8, __ATOMIC_ACQUIRE);
	if (c) {
		return c;
	}
	uint32_t w = g->w, h = g->h;
	uint8_t *out = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) {
		return NULL;
	}
	const uint8_t *src = f->data + g->off;
	/* Масштабный коэффициент отображает 1..4-битное значение покрытия на полный диапазон alpha 0..255. */
	uint32_t bpp = f->bpp, mask = (1u << bpp) - 1u, mul = 255u / mask;
	uint32_t bit = 0;                         /* пиксели упакованы подряд, старший бит первым */
	uint8_t *o = out;
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++, bit += bpp) {
			uint32_t v = (src[bit >> 3] >> (8 - bpp - (bit & 7))) & mask;
			*o++ = (uint8_t)(v * mul);
		}
	}
	uint8_t *expected = NULL;
	if (!__atomic_compare_exchange_n(&g->a8, &expected, out, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		heap_caps_free(out);                  /* другой поток оказался быстрее */
		return expected;
	}
	return out;
}

static const void *font_get_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *buf) {
	const vfont_t *f = (const vfont_t *)d->resolved_font;
	if (!d->gid.index) {
		return NULL;
	}
	vglyph_t *g = &((vfont_t *)f)->glyphs[d->gid.index - 1];
	if (!g->w || !g->h) {
		return NULL;
	}
	const uint8_t *a8 = glyph_a8(f, g);
	if (!a8) {
		return NULL;
	}
	if (!buf) {
		return a8;                            /* статический bitmap: LVGL смешивает напрямую из него */
	}
	uint32_t stride = buf->header.stride;     /* повёрнутый текст и другие нестатические пути: копируем в буфер LVGL */
	uint8_t *out = buf->data;
	for (uint32_t y = 0; y < g->h; y++) {
		memcpy(out + (size_t)y * stride, a8 + (size_t)y * g->w, g->w);
	}
	return buf;
}

/* Разбирает файл и строит шрифт. При ошибке возвращает NULL и устанавливает *err.
 *
 * Формат файла шрифта VESC: 4 неиспользуемых байта, magic "font\0", затем именованные секции
 * (name\0, u32 size, payload): "lmtx" (ascent, descent, line gap), "kern" и "glyphs".
 */
static vfont_t *font_create(const uint8_t *d, uint32_t len, const char **err) {
	if (len < 9 || memcmp(d + 4, "font\0", 5) != 0) {
		*err = "lv-font-load: not a VESC font file";
		return NULL;
	}
	float asc = 0, desc = 0, gap = 0;
	bool have_l = false, have_g = false;
	uint32_t g_off = 0, g_end = 0, k_off = 0, k_end = 0;
	/* Сканирование секций начинается сразу после 9-байтового заголовка (4 + "font\0"). */
	uint32_t p = 9;
	while (p < len) {
		uint32_t e = p;
		while (e < len && d[e]) e++;
		if (e + 5 > len) break;
		const char *name = (const char *)d + p;
		uint32_t size = be32(d + e + 1);
		uint32_t b = e + 5;
		if (size > len - b) {
			*err = "lv-font-load: truncated file";
			return NULL;
		}
		if (!strcmp(name, "lmtx") && size >= 12) {
			asc = f32_auto(be32(d + b));
			desc = f32_auto(be32(d + b + 4));
			gap = f32_auto(be32(d + b + 8));
			have_l = true;
		} else if (!strcmp(name, "kern")) {
			k_off = b; k_end = b + size;
		} else if (!strcmp(name, "glyphs")) {
			g_off = b; g_end = b + size; have_g = true;
		}
		p = b + size;
	}
	if (!have_l || !have_g || g_end - g_off < 8) {
		*err = "lv-font-load: font file has no metrics or glyphs";
		return NULL;
	}
	/* Секция glyph: количество glyph и бит на пиксель (fmt). */
	uint32_t ncodes = be32(d + g_off), fmt = be32(d + g_off + 4);
	if (fmt != 1 && fmt != 2 && fmt != 4) {
		*err = "lv-font-load: unsupported pixel format";
		return NULL;
	}

	/* Защита от переполнения: каждая запись glyph занимает минимум 24 байта, поэтому ncodes ограничено размером секции. */
	if (ncodes > (g_end - g_off - 8) / 24) {
		*err = "lv-font-load: truncated glyph table";
		return NULL;
	}

	vfont_t *f = lv_zalloc(sizeof(vfont_t));
	if (!f) goto oom;
	/* Файл копируется целиком, поэтому Lisp-массив может быть удалён сборщиком мусора; смещения glyph указывают в эту копию. */
	f->data = lv_malloc(len);
	f->glyphs = lv_malloc((ncodes ? ncodes : 1) * sizeof(vglyph_t));
	if (!f->data || !f->glyphs) goto oom;
	memcpy(f->data, d, len);
	f->len = len;
	f->bpp = (uint8_t)fmt;

	/* Каждая запись glyph: code, advance, смещение по x, top, w, h (6 x u32 = 24 байта), затем упакованные пиксели. */
	uint32_t q = g_off + 8;
	for (uint32_t i = 0; i < ncodes; i++) {
		if (q + 24 > g_end) {
			*err = "lv-font-load: truncated glyph table";
			goto bad;
		}
		vglyph_t *g = &f->glyphs[i];
		g->code = be32(d + q);
		g->adv = f32_auto(be32(d + q + 4));
		g->ofs_x = (int16_t)rnd(f32_auto(be32(d + q + 8)));
		g->top = (int16_t)(int32_t)be32(d + q + 12);
		int32_t w = (int32_t)be32(d + q + 16), h = (int32_t)be32(d + q + 20);
		/* Проверка на разумность размера -- защита от повреждённых файлов. */
		if (w < 0 || h < 0 || w > 4096 || h > 4096) {
			*err = "lv-font-load: bad glyph size";
			goto bad;
		}
		g->w = (uint16_t)w;
		g->h = (uint16_t)h;
		g->off = q + 24;
		g->a8 = NULL;
		uint32_t sz = ((uint32_t)w * (uint32_t)h * fmt + 7) / 8;
		if (q + 24 + sz > g_end) {
			*err = "lv-font-load: truncated glyph data";
			goto bad;
		}
		q += 24 + sz;
		f->nglyphs++;
	}
	qsort(f->glyphs, f->nglyphs, sizeof(vglyph_t), cmp_glyph);

	if (k_end - k_off > 4) {
		/* Секция кернинга: для каждого левого символа заголовок (code, count), затем count 12-байтовых записей (правый code, float, ...). */
		uint32_t rows = be32(d + k_off);
		/* Каждая строка занимает минимум 8 байт (code, count); ограничиваем rows размером секции, чтобы не было переполнения. */
		if (rows > (k_end - k_off - 4) / 8) {
			*err = "lv-font-load: bad kerning table";
			goto bad;
		}
		f->kern = lv_malloc((rows ? rows : 1) * sizeof(vkern_t));
		if (!f->kern) goto oom;
		uint32_t kq = k_off + 4;
		for (uint32_t i = 0; i < rows; i++) {
			if (k_end - kq < 8) break;
			uint32_t n = be32(d + kq);
			uint32_t code = n;
			n = be32(d + kq + 4);
			/* Сравнение без переполнения uint32: kq + 8 <= k_end проверено выше. */
			if (n > (k_end - kq - 8) / 12) break;
			f->kern[f->nkern].code = code;
			f->kern[f->nkern].off = kq + 8;
			f->kern[f->nkern].n = n;
			f->nkern++;
			kq += 8 + n * 12;
		}
		qsort(f->kern, f->nkern, sizeof(vkern_t), cmp_kern);
	}

	/* Заполняем lv_font_t: метрики LVGL берутся из ascent/descent/gap файла; подчёркивание выводится из них. */
	int asc_i = rnd(asc), desc_i = rnd(-desc), lh = rnd(asc - desc + gap);
	f->font.get_glyph_dsc = font_get_glyph_dsc;
	f->font.get_glyph_bitmap = font_get_bitmap;
	f->font.release_glyph = NULL;
	f->font.line_height = lh > 0 ? lh : asc_i + desc_i;
	f->font.base_line = desc_i;
	f->font.subpx = LV_FONT_SUBPX_NONE;
	f->font.kerning = LV_FONT_KERNING_NORMAL;
	/* Bitmap кешируются и никогда не освобождаются LVGL, поэтому он может смешивать напрямую из них. */
	f->font.static_bitmap = 1;
	f->font.underline_position = (int8_t)(-(desc_i > 1 ? desc_i / 2 : 1));
	f->font.underline_thickness = (int8_t)(asc_i / 14 > 0 ? asc_i / 14 : 1);
	f->font.dsc = f;
	f->font.fallback = NULL;
	f->font.user_data = NULL;
	f->next = s_fonts;
	s_fonts = f;
	return f;

/* Выходы при ошибке: освобождаем всё, что построено к этому моменту (включая кешированные A8 glyph). */
oom:
	*err = "lv-font-load: out of memory";
bad:
	if (f) {
		if (f->glyphs) { for (uint32_t i = 0; i < f->nglyphs; i++) { if (f->glyphs[i].a8) heap_caps_free(f->glyphs[i].a8); } }
		lv_free(f->data); lv_free(f->glyphs); lv_free(f->kern); lv_free(f);
	}
	return NULL;
}

/* ------------------------------------------------------------------ изображения */

/*
 * Изображение VESC: u16 w, u16 h, u8 bpp (1/2/4), упакованные пиксели, палитра (макс. 2^bpp <= 16 цветов) задаётся из Lisp.
 *
 * Готовые картинки общие: объекты с одинаковыми данными и палитрой ссылаются на одну запись vimg_t
 * (счётчик ссылок, запись освобождается вместе с последним объектом). Картинки до VIMG_ARGB_MAX_PX пикселей
 * один раз переводятся в ARGB8888: LVGL рисует их напрямую, без распаковки палитры на каждом кадре
 * (кеш изображений LVGL в этой прошивке выключен, и индексированная картинка декодировалась при каждой отрисовке).
 * Более крупные остаются индексированными ради памяти (4 байта на пиксель).
 */
#define VIMG_MAX_COLORS  16
#define VIMG_ARGB_MAX_PX (256u * 256u)

typedef struct vimg {
	struct vimg   *next;
	uint32_t       refs;
	uint32_t       key;                    /* FNV-1a исходных байтов */
	uint32_t       len;                    /* длина исходных байтов */
	uint32_t       pal[VIMG_MAX_COLORS];   /* ARGB, 0 = прозрачный */
	uint8_t        bpp;
	uint8_t        argb;                   /* 1: buf = ARGB8888, idx -- отдельная копия индексов */
	uint16_t       w, h;
	lv_image_dsc_t dsc;
	uint8_t       *buf;                    /* dsc.data */
	uint8_t       *idx;                    /* индексы, строки выровнены по байту (для перекраски) */
} vimg_t;

static vimg_t *s_vimgs;   /* все живые записи (только задача LVGL) */

static uint32_t fnv1a(const uint8_t *d, uint32_t n) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) {
		h = (h ^ d[i]) * 16777619u;
	}
	return h;
}

static uint32_t idx_stride(const vimg_t *v) {
	return ((uint32_t)v->w * v->bpp + 7) / 8;
}

/* Сбрасываем запись кеша LVGL перед освобождением пикселей, на которые она может ссылаться. */
static void vimg_unref(vimg_t *v) {
	if (!v || --v->refs > 0) {
		return;
	}
	for (vimg_t **pp = &s_vimgs; *pp; pp = &(*pp)->next) {
		if (*pp == v) {
			*pp = v->next;
			break;
		}
	}
	lv_image_cache_drop(&v->dsc);
	if (v->argb) {
		lv_free(v->idx);
	}
	lv_free(v->buf);
	lv_free(v);
}

/* Хук удаления объекта: отпускает его картинку. */
static void vimg_del_cb(lv_event_t *e) {
	vimg_unref(lv_event_get_user_data(e));
}

/* Находит vimg_t объекта по нашему delete callback в списке его событий. */
static vimg_t *vimg_of(lv_obj_t *obj, lv_event_dsc_t **dsc_out) {
	uint32_t n = lv_obj_get_event_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
		if (lv_event_dsc_get_cb(d) == vimg_del_cb) {
			if (dsc_out) *dsc_out = d;
			return lv_event_dsc_get_user_data(d);
		}
	}
	return NULL;
}

/* Цвета палитры: из Lisp приходят ARGB (0 = прозрачный). */
static void vimg_render(vimg_t *v) {
	uint32_t size = 1u << v->bpp, stride = idx_stride(v), mask = size - 1u;
	if (!v->argb) {
		lv_color32_t *pal = (lv_color32_t *)v->buf;
		for (uint32_t i = 0; i < size; i++) {
			uint32_t c = v->pal[i];
			pal[i].blue = c & 0xFF;
			pal[i].green = (c >> 8) & 0xFF;
			pal[i].red = (c >> 16) & 0xFF;
			pal[i].alpha = (c >> 24) & 0xFF;
		}
		return;
	}
	lv_color32_t lut[VIMG_MAX_COLORS];
	for (uint32_t i = 0; i < size; i++) {
		uint32_t c = v->pal[i];
		lut[i].blue = c & 0xFF;
		lut[i].green = (c >> 8) & 0xFF;
		lut[i].red = (c >> 16) & 0xFF;
		lut[i].alpha = (c >> 24) & 0xFF;
	}
	lv_color32_t *out = (lv_color32_t *)v->buf;
	for (uint32_t y = 0; y < v->h; y++) {
		const uint8_t *row = v->idx + y * stride;
		for (uint32_t x = 0; x < v->w; x++) {
			uint32_t bit = x * v->bpp;
			uint32_t px = (row[bit >> 3] >> (8 - v->bpp - (bit & 7))) & mask;
			*out++ = lut[px];
		}
	}
}

/* Новая запись с индексами idx (копируются) и палитрой pal; refs = 1. NULL при нехватке памяти. */
static vimg_t *vimg_new(uint32_t key, uint32_t len, uint16_t w, uint16_t h, uint8_t bpp,
		const uint8_t *idx, const uint32_t *pal) {
	vimg_t *v = lv_zalloc(sizeof(vimg_t));
	if (!v) {
		return NULL;
	}
	v->key = key; v->len = len; v->w = w; v->h = h; v->bpp = bpp;
	memcpy(v->pal, pal, sizeof(v->pal));
	uint32_t stride = idx_stride(v), isz = stride * h, pal_bytes = (1u << bpp) * 4;
	v->argb = (uint32_t)w * h <= VIMG_ARGB_MAX_PX;
	if (v->argb) {
		v->idx = lv_malloc(isz ? isz : 1);
		v->buf = lv_malloc((size_t)w * h * 4 + 4);
	} else {
		v->buf = lv_malloc(pal_bytes + isz);
		v->idx = v->buf ? v->buf + pal_bytes : NULL;
	}
	if (!v->buf || !v->idx) {
		if (v->argb) lv_free(v->idx);
		lv_free(v->buf);
		lv_free(v);
		return NULL;
	}
	memcpy(v->idx, idx, isz);
	vimg_render(v);
	v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	v->dsc.header.w = w;
	v->dsc.header.h = h;
	if (v->argb) {
		v->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
		v->dsc.header.stride = (uint32_t)w * 4;
		v->dsc.data_size = (uint32_t)w * h * 4;
	} else {
		v->dsc.header.cf = bpp == 1 ? LV_COLOR_FORMAT_I1 : bpp == 2 ? LV_COLOR_FORMAT_I2 : LV_COLOR_FORMAT_I4;
		v->dsc.header.stride = stride;
		v->dsc.data_size = pal_bytes + isz;
	}
	v->dsc.data = v->buf;
	v->refs = 1;
	v->next = s_vimgs;
	s_vimgs = v;
	return v;
}

/* Готовая запись с теми же исходными данными и палитрой (refs++), или NULL. */
static vimg_t *vimg_find(uint32_t key, uint32_t len, uint16_t w, uint16_t h, uint8_t bpp, const uint32_t *pal) {
	for (vimg_t *v = s_vimgs; v; v = v->next) {
		if (v->key == key && v->len == len && v->w == w && v->h == h && v->bpp == bpp &&
				memcmp(v->pal, pal, sizeof(v->pal)) == 0) {
			v->refs++;
			return v;
		}
	}
	return NULL;
}

/* Ставит объекту картинку v (ссылка уже взята) и отпускает прежнюю. Прежняя освобождается только после
 * установки новой, поэтому LVGL никогда не рисует из освобождённой памяти. */
static void vimg_attach(lv_obj_t *obj, vimg_t *v) {
	lv_event_dsc_t *old_dsc = NULL;
	vimg_t *old = vimg_of(obj, &old_dsc);
	lv_obj_add_event_cb(obj, vimg_del_cb, LV_EVENT_DELETE, v);
	lv_image_set_src(obj, &v->dsc);
	if (old) {
		lv_obj_remove_event_dsc(obj, old_dsc);
		vimg_unref(old);
	}
}

/*
 * Блок аргументов, передаваемый в задачу LVGL (задача eval тем временем блокируется, поэтому данные на стеке остаются валидными).
 */
typedef struct {
	int32_t        obj;
	const uint8_t *data;
	uint32_t       len;
	uint32_t       pal[VIMG_MAX_COLORS];
	uint32_t       npal;
	int32_t        out[2];
	const char    *err;
} img_args_t;

/* Проверяет 5-байтовый заголовок и то, что пиксельные данные полны. */
static bool img_header(const uint8_t *d, uint32_t len, uint32_t *w, uint32_t *h, uint32_t *bpp) {
	if (len < 5) return false;
	*w = ((uint32_t)d[0] << 8) | d[1];
	*h = ((uint32_t)d[2] << 8) | d[3];
	*bpp = d[4];
	if (*bpp != 1 && *bpp != 2 && *bpp != 4) return false;
	/* Ограничиваем размеры и считаем в 64 битах, чтобы маленький буфер не прошёл проверку длины из-за переполнения. */
	if (*w > 4096 || *h > 4096) return false;
	return (uint64_t)len >= 5 + ((uint64_t)*w * *h * *bpp + 7) / 8;
}

/* палитра аргументов -> 16 записей ARGB (не заданные = прозрачные) */
static void args_palette(const img_args_t *a, uint32_t out[VIMG_MAX_COLORS]) {
	for (uint32_t i = 0; i < VIMG_MAX_COLORS; i++) {
		out[i] = i < a->npal ? a->pal[i] : 0;
	}
}

/* Задача LVGL: ставит объекту картинку VESC (общую с другими объектами, если данные и палитра те же). */
static void image_set_cb(void *arg) {
	img_args_t *a = arg;
	lv_obj_t *obj = lvbr_handle_get(LVBR_HK_OBJ, a->obj);
	if (!obj) {
		a->err = "lv: invalid or deleted handle";
		return;
	}
	/* Объект должен быть lv_image, иначе lv_image_set_src работал бы с чужой структурой. */
	if (!lv_obj_check_type(obj, &lv_image_class)) {
		a->err = "lv-image-set-vesc: object is not an image";
		return;
	}
	uint32_t w, h, bpp;
	if (!img_header(a->data, a->len, &w, &h, &bpp)) {
		a->err = "lv-image-set-vesc: not a VESC image (u16 w, u16 h, u8 bpp 1/2/4, pixels)";
		return;
	}
	uint32_t pal[VIMG_MAX_COLORS];
	args_palette(a, pal);
	uint32_t key = fnv1a(a->data, a->len);
	vimg_t *v = vimg_find(key, a->len, (uint16_t)w, (uint16_t)h, (uint8_t)bpp, pal);
	if (!v) {
		/* VESC упаковывает пиксели подряд; здесь каждая строка начинается с границы байта */
		uint32_t stride = (w * bpp + 7) / 8, isz = stride * h;
		uint8_t *idx = lv_malloc(isz ? isz : 1);
		if (!idx) {
			a->err = "lv-image-set-vesc: out of memory";
			return;
		}
		memset(idx, 0, isz);
		const uint8_t *src = a->data + 5;
		uint32_t bit = 0, mask = (1u << bpp) - 1u;
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++, bit += bpp) {
				uint32_t px = (src[bit >> 3] >> (8 - bpp - (bit & 7))) & mask;
				idx[y * stride + ((x * bpp) >> 3)] |= (uint8_t)(px << (8 - bpp - ((x * bpp) & 7)));
			}
		}
		v = vimg_new(key, a->len, (uint16_t)w, (uint16_t)h, (uint8_t)bpp, idx, pal);
		lv_free(idx);
		if (!v) {
			a->err = "lv-image-set-vesc: out of memory";
			return;
		}
	}
	vimg_attach(obj, v);
}

/* Задача LVGL: перекрашивает картинку только этого объекта (другие объекты с той же картинкой не меняются). */
static void image_colors_cb(void *arg) {
	img_args_t *a = arg;
	lv_obj_t *obj = lvbr_handle_get(LVBR_HK_OBJ, a->obj);
	if (!obj) {
		a->err = "lv: invalid or deleted handle";
		return;
	}
	vimg_t *cur = vimg_of(obj, NULL);
	if (!cur) {
		a->err = "lv-image-set-colors: the image has no VESC picture (use lv-image-set-vesc first)";
		return;
	}
	uint32_t pal[VIMG_MAX_COLORS];
	args_palette(a, pal);
	if (memcmp(cur->pal, pal, sizeof(pal)) == 0) {
		return;
	}
	vimg_t *v = vimg_find(cur->key, cur->len, cur->w, cur->h, cur->bpp, pal);
	if (!v) {
		v = vimg_new(cur->key, cur->len, cur->w, cur->h, cur->bpp, cur->idx, pal);
		if (!v) {
			a->err = "lv-image-set-colors: out of memory";
			return;
		}
	}
	vimg_attach(obj, v);
	lv_obj_invalidate(obj);
}

/* список цветов: число 0xRRGGBB = непрозрачный, nil = прозрачный */
/* Максимум 16 записей (наибольшая палитра -- 4 bpp); каждая хранится как ARGB с alpha 0xFF, nil -- как 0 (прозрачный). */
static bool parse_colors(lbm_value l, img_args_t *a) {
	a->npal = 0;
	if (lbm_is_symbol_nil(l)) {
		return true;
	}
	if (!lbm_is_cons(l)) {
		lbm_set_error_reason("lv: colours must be a list like (list nil 0x555555 0xFFFFFF)");
		return false;
	}
	for (; lbm_is_cons(l); l = lbm_cdr(l)) {
		lbm_value c = lbm_car(l);
		if (a->npal >= VIMG_MAX_COLORS) {
			lbm_set_error_reason("lv: at most 16 colours");
			return false;
		}
		if (lbm_is_symbol_nil(c)) {
			a->pal[a->npal++] = 0;
		} else if (lbm_is_number(c)) {
			a->pal[a->npal++] = 0xFF000000u | ((uint32_t)lbm_dec_as_u32(c) & 0xFFFFFF);
		} else {
			lbm_set_error_reason("lv: colour must be a number or nil");
			return false;
		}
	}
	return true;
}

/* Заимствует сырые байты Lisp-массива (без копирования; действительны только пока задача eval заблокирована в lvbr_run). */
static bool arg_bytes(lbm_value v, const uint8_t **p, uint32_t *n) {
	if (!lbm_is_array_r(v)) {
		lbm_set_error_reason("lv: expected a byte array (imported .bin)");
		return false;
	}
	lbm_array_header_t *h = lbm_dec_array_r(v);
	if (!h || !h->data) {
		lbm_set_error_reason("lv: expected a byte array (imported .bin)");
		return false;
	}
	*p = (const uint8_t *)h->data;
	*n = (uint32_t)h->size;
	return true;
}

/* (lv-image-set-vesc img data colours) */
/* Расширение задачи eval: разбирает аргументы, затем выполняет работу в задаче LVGL через lvbr_run(). */
static lbm_value ext_image_set_vesc(lbm_value *args, lbm_uint argn) {
	img_args_t a;
	memset(&a, 0, sizeof(a));
	if (argn < 2 || argn > 3 || !lvbr_arg_i32(args[0], &a.obj) || !arg_bytes(args[1], &a.data, &a.len)) {
		lbm_set_error_reason("lv-image-set-vesc: (img data [colours])");
		return ENC_SYM_TERROR;
	}
	if (argn == 3 && !parse_colors(args[2], &a)) {
		return ENC_SYM_TERROR;
	}
	/* Палитра по умолчанию: индекс 0 прозрачный, все остальные белые. */
	if (argn == 2) {                      /* по умолчанию: индекс 0 прозрачный, остальные белые */
		a.pal[0] = 0;
		for (int i = 1; i < VIMG_MAX_COLORS; i++) a.pal[i] = 0xFFFFFFFFu;
		a.npal = VIMG_MAX_COLORS;
	}
	if (!lvbr_run(image_set_cb, &a) || a.err) {
		lbm_set_error_reason(a.err ? a.err : "lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

/* (lv-image-set-colors img colours) */
static lbm_value ext_image_set_colors(lbm_value *args, lbm_uint argn) {
	img_args_t a;
	memset(&a, 0, sizeof(a));
	if (argn != 2 || !lvbr_arg_i32(args[0], &a.obj) || !parse_colors(args[1], &a)) {
		lbm_set_error_reason("lv-image-set-colors: (img colours)");
		return ENC_SYM_TERROR;
	}
	if (!lvbr_run(image_colors_cb, &a) || a.err) {
		lbm_set_error_reason(a.err ? a.err : "lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}

/* (lv-vesc-image-size data) -> (w h)  (LVGL не задействован) */
/* Только чтение заголовка, поэтому переход в задачу LVGL не нужен. */
static lbm_value ext_vesc_image_size(lbm_value *args, lbm_uint argn) {
	const uint8_t *p;
	uint32_t n, w, h, bpp;
	if (argn != 1 || !arg_bytes(args[0], &p, &n)) {
		return ENC_SYM_TERROR;
	}
	if (!img_header(p, n, &w, &h, &bpp)) {
		lbm_set_error_reason("lv-vesc-image-size: not a VESC image");
		return ENC_SYM_EERROR;
	}
	return lbm_heap_allocate_list_init(2, lbm_enc_i((int32_t)w), lbm_enc_i((int32_t)h));
}


/* ------------------------------------------------------------------- линии */
/* lv_line хранит указатель на свои точки, поэтому массив живёт столько же, сколько объект. */

#if LV_USE_LINE
/* vline_t: собственный массив точек (flexible member), освобождается vline_del_cb вместе с объектом. */
typedef struct { uint32_t n; lv_point_precise_t pts[]; } vline_t;

static void vline_del_cb(lv_event_t *e) {
	lv_free(lv_event_get_user_data(e));
}

/* Находит vline_t объекта через его delete callback (тот же приём, что в vimg_of). */
static vline_t *vline_of(lv_obj_t *obj, lv_event_dsc_t **dsc_out) {
	uint32_t n = lv_obj_get_event_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
		if (lv_event_dsc_get_cb(d) == vline_del_cb) {
			if (dsc_out) *dsc_out = d;
			return lv_event_dsc_get_user_data(d);
		}
	}
	return NULL;
}

/* Ограничение числа точек сохраняет line_args_t (размещается на стеке задачи eval) небольшим. */
#define VLINE_MAX 64
typedef struct { int32_t obj; int32_t n; int32_t xy[VLINE_MAX * 2]; const char *err; } line_args_t;

static void line_points_cb(void *arg) {
	line_args_t *a = arg;
	lv_obj_t *obj = lvbr_handle_get(LVBR_HK_OBJ, a->obj);
	if (!obj) {
		a->err = "lv: invalid or deleted handle";
		return;
	}
	lv_event_dsc_t *old_dsc = NULL;
	vline_t *v = vline_of(obj, &old_dsc);
	if (v && v->n == (uint32_t)a->n) {            /* тот же размер: перезаписываем на месте */
		for (int i = 0; i < a->n; i++) { v->pts[i].x = a->xy[2 * i]; v->pts[i].y = a->xy[2 * i + 1]; }
		lv_line_set_points(obj, v->pts, v->n);
		return;
	}
	/* Размер изменился: сначала выделяем новый массив, переключаем line на него и только потом освобождаем старый. */
	vline_t *nv = lv_malloc(sizeof(vline_t) + sizeof(lv_point_precise_t) * (size_t)a->n);
	if (!nv) {
		a->err = "lv-line-set-points: out of memory";
		return;
	}
	nv->n = (uint32_t)a->n;
	for (int i = 0; i < a->n; i++) { nv->pts[i].x = a->xy[2 * i]; nv->pts[i].y = a->xy[2 * i + 1]; }
	lv_obj_add_event_cb(obj, vline_del_cb, LV_EVENT_DELETE, nv);
	lv_line_set_points(obj, nv->pts, nv->n);
	if (v) {
		lv_obj_remove_event_dsc(obj, old_dsc);
		lv_free(v);
	}
}

/* (lv-line-set-points line (list x0 y0 x1 y1 ...)) */
static lbm_value ext_line_set_points(lbm_value *args, lbm_uint argn) {
	line_args_t a;
	memset(&a, 0, sizeof(a));
	if (argn != 2 || !lvbr_arg_i32(args[0], &a.obj) || !lbm_is_cons(args[1])) {
		lbm_set_error_reason("lv-line-set-points: (line (list x0 y0 x1 y1 ...))");
		return ENC_SYM_TERROR;
	}
	int32_t cnt = 0;
	for (lbm_value l = args[1]; lbm_is_cons(l); l = lbm_cdr(l)) {
		if (cnt >= VLINE_MAX * 2) {
			lbm_set_error_reason("lv-line-set-points: at most 64 points");
			return ENC_SYM_TERROR;
		}
		if (!lvbr_arg_i32(lbm_car(l), &a.xy[cnt++])) {
			return ENC_SYM_TERROR;
		}
	}
	if (cnt < 2 || (cnt & 1)) {
		lbm_set_error_reason("lv-line-set-points: need an even number of values");
		return ENC_SYM_TERROR;
	}
	a.n = cnt / 2;
	if (!lvbr_run(line_points_cb, &a) || a.err) {
		lbm_set_error_reason(a.err ? a.err : "lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return ENC_SYM_TRUE;
}
#endif /* LV_USE_LINE */

/* -------------------------------------------------------------- вход шрифта */

typedef struct {
	const uint8_t *data;
	uint32_t       len;
	int32_t        handle;
	const char    *err;
} font_args_t;

static void font_load_cb(void *arg) {
	font_args_t *a = arg;
	vfont_t *f = font_create(a->data, a->len, &a->err);
	if (!f) {
		return;
	}
	a->handle = lvbr_handle_for(LVBR_HK_FONT, &f->font);
	if (!a->handle) {
		a->err = "lv-font-load: out of handles";
		/* Шрифт только что добавлен в голову s_fonts: отвязываем и освобождаем его, чтобы он не утёк. */
		s_fonts = f->next;
		for (uint32_t i = 0; i < f->nglyphs; i++) { if (f->glyphs[i].a8) heap_caps_free(f->glyphs[i].a8); }
		lv_free(f->data); lv_free(f->glyphs); lv_free(f->kern); lv_free(f);
	}
}

/* (lv-font-load data) -> handle шрифта (FONT) */
/* Шрифты копируются в память со стороны LVGL и живут до lvbr_assets_reset(); повторная загрузка одного и того же файла создаёт два шрифта. */
static lbm_value ext_font_load(lbm_value *args, lbm_uint argn) {
	font_args_t a = { 0 };
	if (argn != 1 || !arg_bytes(args[0], &a.data, &a.len)) {
		lbm_set_error_reason("lv-font-load: (data)  data = imported font .bin");
		return ENC_SYM_TERROR;
	}
	if (!lvbr_run(font_load_cb, &a) || a.err) {
		lbm_set_error_reason(a.err ? a.err : "lv: LVGL task not available");
		return ENC_SYM_EERROR;
	}
	return lbm_enc_i(a.handle);
}

/* Вызывается из core в задаче LVGL при перезапуске Lisp-программы, после удаления объектов
 * (так что ни одна label больше не использует эти шрифты). */
void lvbr_assets_reset(void) {
	while (s_fonts) {
		vfont_t *f = s_fonts;
		s_fonts = f->next;
		for (uint32_t i = 0; i < f->nglyphs; i++) { if (f->glyphs[i].a8) heap_caps_free(f->glyphs[i].a8); }
		lv_free(f->data); lv_free(f->glyphs); lv_free(f->kern); lv_free(f);
	}
}

/* Регистрируется в lvbr_core.c через s_groups[]. */
const lvbr_ext_t lvbr_assets_table[] = {
	{ "lv-font-load",       ext_font_load },
	{ "lv-image-set-vesc",  ext_image_set_vesc },
	{ "lv-image-set-colors", ext_image_set_colors },
	{ "lv-vesc-image-size", ext_vesc_image_size },
#if LV_USE_LINE
	{ "lv-line-set-points", ext_line_set_points },
#endif
};
const unsigned lvbr_assets_count = sizeof(lvbr_assets_table) / sizeof(lvbr_assets_table[0]);
