#ifndef MAIN_HWCONF_P4_DASHBOARD_LVGL_BRIDGE_H_
#define MAIN_HWCONF_P4_DASHBOARD_LVGL_BRIDGE_H_

#include <stdbool.h>

/* Вызывается прошивкой (lispif_add_ext_load_callback) каждый раз, когда среда выполнения Lisp
 * (пере)запускается: регистрирует расширения lv-* и константы и
 * очищает то, что предыдущая программа оставила на экране. */
void dashboard_lvgl_ext_load(bool main_found);

#endif /* MAIN_HWCONF_P4_DASHBOARD_LVGL_BRIDGE_H_ */
