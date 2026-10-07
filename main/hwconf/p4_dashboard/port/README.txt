Переносим P4 DASHBOARD на чистый vesc_express (github.com/vedderb/vesc_express)
==========================================================================

От папки p4_dashboard проект отличается только тремя вещами:

1. main/CMakeLists.txt   - добавить lvgl_bridge.c и bridge/*.c, include-папки и LV_CONF_INCLUDE_SIMPLE
2. main/idf_component.yml - добавить зависимость lvgl/lvgl (st7701 и gt911 в апстриме уже есть)
3. sdkconfig по умолчанию - апстрим берёт sdkconfig.defaults.<HW_TARGET> из корня проекта

Всё остальное (main.c, hw.h, lispif.c, flash_helper.c, lispBM, корневой
CMakeLists.txt) совпадает с апстримом (сверял с коммитом c085911 от 17.09.2026).

Шаги (из корня чистого проекта):

  cp -r <старый проект>/main/hwconf/p4_dashboard main/hwconf/
  git apply main/hwconf/p4_dashboard/port/upstream.patch
  cp main/hwconf/p4_dashboard/sdkconfig.defaults sdkconfig.defaults.esp32p4_dashboard

  idf.py -B build_p4 -DHW_NAME="P4 DASHBOARD" -DSDKCONFIG=build_p4/sdkconfig build
  idf.py -B build_p4 -DHW_NAME="P4 DASHBOARD" -DSDKCONFIG=build_p4/sdkconfig -p /dev/cu.usbmodem1101 flash

Если build_p4 уже есть, но собран в другом проекте, удали его: sdkconfig
в нём пересоздастся из defaults.
