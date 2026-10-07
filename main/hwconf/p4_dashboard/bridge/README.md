# Мост LispBM ↔ LVGL 9.5 (P4 Dashboard)

Lisp видит LVGL **один в один**: имя функции — это C-имя, где `_` заменено на `-`,
константы — как в C. Никаких особых функций «поверх» LVGL нет.

```lisp
(def ui-speed (lv-label-create ui-screen))              ; lv_label_create(parent)
(lv-obj-set-pos ui-speed 100 50)                        ; lv_obj_set_pos(obj, x, y)
(lv-label-set-text ui-speed "SPEED")
(lv-obj-set-style-text-color ui-speed (lv-color-hex 0xFFFFFF) LV_PART_MAIN)
(lv-obj-set-style-text-font  ui-speed font-montserrat-24 LV_PART_MAIN)
```

## Правила API

* **Хэндлы.** Объекты, стили, шрифты, серии графика — маленькие целые числа. Хэндл удалённого
  объекта становится недействительным (ошибка, а не чужой объект). `nil` = `NULL`
  (только там, где C допускает NULL: `parent` в `*_create`).
* **Цвет** — число `0xRRGGBB`; `(lv-color-hex 0xRRGGBB)` оставлен для совместимости с C-записью.
* **Проценты и размеры:** `(lv-pct 50)`, `LV_SIZE_CONTENT`.
* **Тема LVGL:** `(lv-theme-set 0x2196F3 0x03A9F4 nil)` — основной и акцентный цвета, `t` = тёмная тема (включается в `ui-init` сгенерированного кода).
* **Шрифты:** `font-default`, `font-montserrat-14/16/20/24/32/48` (какие есть, определяет
  `sdkconfig.defaults`: `CONFIG_LV_FONT_MONTSERRAT_NN=y`).
* **Регистр.** LispBM читает символы в нижнем регистре, поэтому `LV_PART_MAIN` и `lv_part_main`
  — одно и то же.
* **Переменные.** В LispBM `setq` работает только для уже созданной переменной. Новая
  глобальная переменная создаётся через `(def имя значение)`.
* **Строки** возвращаются копией (до 2047 символов).
* **График:** `(lv-chart-set-series-values chart ser (list ...))` заменяет точки series: точка i = значение i,
  точки за концом списка пустые.
* Любая ошибка в аргументах — обычная ошибка LispBM (`trap` её ловит), плата не падает.
* При (пере)запуске Lisp-программы мост сам удаляет всё, что построила прошлая: объекты,
  экраны, стили, обработчики событий; яркость подсветки возвращается на 100.

## События

LVGL вызывает обработчики в своей задаче, а Lisp работает в своей, поэтому событие кладётся в
очередь, а программа его забирает:

```lisp
(lv-obj-add-event-cb btn 'on-click LV_EVENT_CLICKED)          ; 'on-click — имя Lisp-функции
(lv-obj-add-event-cb btn 'on-click LV_EVENT_CLICKED 'speed)   ; + user-data (символ, число или nil)

(defun on-click (e)
  (let ((code   (lv-event-get-code e))
        (target (lv-event-get-target-obj e))
        (ud     (lv-event-get-user-data e)))
    ...))

(defun ui-poll ()                                  ; обработать одно событие, t если оно было
  (let ((e (lv-event-poll)))                       ; nil, если очередь пуста
    (if e (progn (eval (list (lv-event-get-cb e) (list 'quote e))) t) nil)))

(defun ui-run ()                                   ; хвостовая рекурсия, стек не растёт
  (if (not (ui-poll)) (lv-event-wait 0.02))        ; спит до события (просыпается сразу), не дольше 20 мс
  (ui-run))

;; общий обработчик касаний: каждое нажатие/отпускание экрана, на каком бы объекте оно ни было
(lv-touch-hook 'on-touch)                  ; on-touch получает LV_EVENT_PRESSED / LV_EVENT_RELEASED
(lv-event-get-x e) (lv-event-get-y e)      ; точка касания (и у событий ввода объектов)
(lv-touch-get)                             ; -> (pressed x y) прямо сейчас, для удержания и т.п.
(lv-touch-hook nil)                        ; выключить

(lv-obj-remove-event-cb btn 'on-click)                     ; снять подписки -> их число
(lv-obj-remove-event-cb btn 'on-click LV_EVENT_CLICKED)    ; только с этим фильтром
```

Подписок можно сколько угодно (ограничение — только память), они освобождаются вместе с объектом.
Подписка на `LV_EVENT_DELETE` получает прежний хэндл объекта как идентификатор (вызовы `lv-*` с ним
вернут ошибку). Очередь — 128 событий; при переполнении сначала теряются непрерывные
(`PRESSING`, `SCROLL`...), потом самые старые; счётчик потерь — 12-й элемент `(lv-perf)`,
число подписок — 13-й.

`LV_EVENT_ALL` пропускает только события ввода/значения/экрана (не отрисовку и раскладку).
Повторяющиеся события (`PRESSING`, `VALUE_CHANGED`, `SCROLL`) пока не прочитаны — сливаются в одно.

## Шрифты и картинки (формат VESC .bin)

Те же файлы, что делались для старого Lisp-интерфейса (`font/barlow-bold-24-4c.bin`,
`assets/icon-wifi_32.bin`), подключаются через обычный `import`:

```lisp
(import "font/barlow-bold-24-4c.bin" 'fb24)
(def f24 (lv-font-load fb24))                              ; -> хэндл шрифта
(lv-obj-set-style-text-font label f24 LV_PART_MAIN)

(import "assets/icon-wifi_32.bin" 'ic-wifi)
(def img (lv-image-create scr))
(lv-image-set-vesc img ic-wifi (list nil 0x555555 0xAAAAAA 0xFFFFFF))  ; цвет на каждый индекс палитры, nil = прозрачный
(lv-image-set-colors img (list nil 0x00FF00 0x00AA00 0xFFFFFF))        ; перекраска на лету
(lv-vesc-image-size ic-wifi)                                           ; -> (w h)
```

Данные копируются; шрифты живут до перезапуска Lisp, картинка — пока жив её объект.
Реализация: `lvbr_assets.c`; проверка: `test/tests/assets.lisp` (настоящие файлы Barlow и иконка).

## Как устроено

| файл | что делает | когда менять |
|---|---|---|
| `lv_api.def` | **список функций LVGL**: одна строка `F(RET, lv_имя, ARG, ...)` на функцию | чтобы добавить/убрать функцию |
| `lv_consts.def` | список констант `K(LV_ИМЯ)` | чтобы добавить/убрать константу |
| `lv_fonts.def` | встроенные шрифты `FONT(NN)` | почти никогда |
| `lvbr_extra.c` | функции, которые не выразить строкой (список для графика, подсветка…) | чтобы добавить «ручную» функцию |
| `lvbr_events.c` | события | редко |
| `lvbr_assets.c` | шрифты и картинки VESC .bin | редко |
| `lvbr_api.c`, `lvbr_core.c`, `lvbr.h` | ядро: хэндлы, перевод аргументов, вызов в LVGL-задаче, сброс | не менять |
| `tools/gen_api.py` | читает заголовки LVGL и сам пишет строки `.def` | — |
| `test/` | стенд на компьютере: настоящий LispBM + LVGL + мост | после любых изменений |

Все вызовы LVGL выполняются в LVGL-задаче (`dashboard_lvgl_call`), а в задаче Lisp (стек ≈3 КБ)
только переводятся аргументы.

## Как добавить

**Функцию LVGL.** Одной командой (читает прототип из заголовков, сама подбирает типы и ставит
строку под нужный `#if LV_USE_…`):

```
python3 bridge/tools/gen_api.py --lvgl <путь>/lvgl/src add lv_label_set_long_mode lv_bar_set_value
```

или вручную строкой в `lv_api.def`: `F(V, lv_obj_set_pos, OBJ, I, I)`.
Типы: `V` — void (только результат), `I` int32, `U` uint32, `B` bool, `S` строка, `C` цвет,
`OBJ` объект, `OBJN` объект или nil, `STYLE`, `FONT`, `SER` (серия графика), `CUR`, `GRP`.
Если у функции есть тип, которого в списке нет (структура, callback, массив), генератор скажет
об этом — такая функция пишется вручную в `lvbr_extra.c`.

**Константу.** Строка `K(LV_ИМЯ)` в `lv_consts.def` (или имя перечисления в
`tools/consts_seed.txt` и `gen_api.py consts`).

**Ручную функцию.** Пишете `static lbm_value ext_xxx(lbm_value *args, lbm_uint argn)` в
`lvbr_extra.c` и добавляете одну строку в таблицу `lvbr_extra_table[]` внизу файла.
Работа с LVGL — только внутри функции, переданной в `lvbr_run()`.

**Новый вид хэндла** (например, анимация): одна строка в `LVBR_HKINDS` (`lvbr.h`) и её имя
можно использовать в `lv_api.def`.

**Новый файл с ручными функциями:** строка в `s_groups[]` (`lvbr_core.c`) и файл в
`CMakeLists.txt`.

## Ограничения

* Таблица расширений LispBM: `USER_EXTENSION_STORAGE_SIZE` в `hw_p4_dashboard.h` (сейчас 768,
  занято ≈570). Каждая новая функция — ещё одно место.
* Каждая константа — глобальный символ LispBM, занимает ≈2 ячейки кучи Lisp (350 констант ≈ 700
  ячеек). Лишние константы лучше убирать из `lv_consts.def`.
* Хэндлов 4095 одновременно; очередь событий 128; число подписок ограничено только памятью.
* Вызов из Lisp, который задача LVGL не взяла за 3 с (зависла отрисовка), отменяется с ошибкой
  `lv: LVGL task not available`, а не ждёт вечно.
* Не поддерживаются функции с указателями на данные (картинки `lv_image_set_src`, массивы,
  структуры) — они делаются вручную, когда понадобятся.
