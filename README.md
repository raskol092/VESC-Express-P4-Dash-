# VESC Express P4 Dashboard

Порт [vesc_express](https://github.com/vedderb/vesc_express) на плату
**Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3** с экраном 800×480 и **мостом LispBM ↔ LVGL 9.5**:
Lisp-программы строят интерфейс обычными функциями LVGL (`lv-label-create`, `lv-obj-set-pos`, …).

*English: a vesc_express port for the Waveshare ESP32-P4 4.3" touch board with a LispBM ↔ LVGL 9.5 bridge,
so Lisp programs can build their UI with plain LVGL calls.*

## Что внутри

```
main/
  CMakeLists.txt, idf_component.yml        # vesc_express main/ с подключённым мостом и зависимостью lvgl
  hwconf/p4_dashboard/
    hw_p4_dashboard.[ch]                   # плата: MIPI-DSI ST7701, тач GT911, подсветка, PPA, тройная буферизация
    lvgl_bridge.[ch]                       # точка входа моста при (пере)запуске Lisp
    bridge/                                # мост: lv_api.def / lv_consts.def / lvbr_*.c  (см. bridge/README.md)
    lisp/nav_test.lisp                     # пример: жесты, страницы, шторка
    lvgl/lv_conf.h, partition_ota_32mb.csv, sdkconfig.defaults
    port/                                  # как перенести порт на чистый vesc_express (upstream.patch)
scripts/apply_to_vesc_express.sh           # применить порт к чистому vesc_express
```

Разрешение панели 480×800, экран повёрнут (`LCD_ROTATION 1`) → **800×480 альбомный**.

## Сборка

Нужен ESP-IDF для ESP32-P4 и чистый `vesc_express`. Порт накатывается скриптом (проверено на `vesc_express` c085911: патч применяется без правок):

```bash
scripts/apply_to_vesc_express.sh /path/to/vesc_express
cd /path/to/vesc_express
idf.py -B build_p4 -DHW_NAME="P4 DASHBOARD" -DSDKCONFIG=build_p4/sdkconfig build
idf.py -B build_p4 -DHW_NAME="P4 DASHBOARD" -DSDKCONFIG=build_p4/sdkconfig -p /dev/ttyACM0 flash
```

Если `build_p4` собран в другом проекте, удалите его: `sdkconfig` пересоздастся из defaults.
Подробнее — `main/hwconf/p4_dashboard/port/README.txt`.

## Интерфейс из lvgl-editor

[lvgl-editor](https://github.com/raskol092/lvgl-editor) генерирует проект под этот мост:

1. В редакторе создайте проект с холстом **800×480**.
2. Нажмите **Скачать ZIP** → `main.lisp`, папка `ui/` (`ui.lisp`, `ui_events.lisp`, `ui_logic.lisp`),
   а также `assets/*.bin` и `font/*.bin` для картинок и шрифтов.
3. Откройте `main.lisp` в VESC Tool (вкладка Lisp) и нажмите **Upload** — строки `(import …)` упакуют файлы.

Мост принимает данные в формате VESC `.bin` (`lv-image-set-vesc`, `lv-font-load`), поэтому ресурсы
редактор конвертирует сам.

## Мост LispBM ↔ LVGL

Имя функции — это C-имя LVGL с `-` вместо `_` (`lv_obj_set_pos` → `lv-obj-set-pos`), константы остаются
как в C (`LV_PART_MAIN`). События ставятся в очередь и разбираются в Lisp-потоке (`lv-event-poll`,
`lv-event-wait`). Список функций — `bridge/lv_api.def`, констант — `bridge/lv_consts.def`;
как добавить свою — в `bridge/README.md`.

## Что в `main/CMakeLists.txt` и `main/idf_component.yml`

Это эталонные копии файлов vesc_express с уже подключённым портом. Копировать их поверх своего проекта не нужно:
`apply_to_vesc_express.sh` вносит те же изменения патчем (`port/upstream.patch`) и не трогает остальное.

## Известные ограничения

- В архиве нет `bridge/tools/gen_api.py` и тестового стенда `bridge/test/`, на которые ссылается `bridge/README.md`.
- Таблица расширений LispBM: `USER_EXTENSION_STORAGE_SIZE` = 768 (занято ≈570).
- Все разделы флеша держатся ниже 16 МБ: выше `mmap` на этой плате «заворачивается» (см. `partition_ota_32mb.csv`).
- `sdkconfig.defaults` — полный дамп menuconfig, а не только отличия от умолчаний.
