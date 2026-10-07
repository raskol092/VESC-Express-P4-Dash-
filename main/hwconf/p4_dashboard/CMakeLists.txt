# Dashboard-local build additions. Included only for HW_TARGET=esp32p4_dashboard.
set(P4_DASHBOARD_EXTRA_SRCS
    "hwconf/p4_dashboard/lvgl_bridge.c"
    "hwconf/p4_dashboard/bridge/lvbr_core.c"
    "hwconf/p4_dashboard/bridge/lvbr_api.c"
    "hwconf/p4_dashboard/bridge/lvbr_events.c"
    "hwconf/p4_dashboard/bridge/lvbr_extra.c"
    "hwconf/p4_dashboard/bridge/lvbr_assets.c"
)

set(P4_DASHBOARD_EXTRA_INCLUDES
    "hwconf/p4_dashboard"
    "hwconf/p4_dashboard/lvgl"
)

set(P4_DASHBOARD_EXTRA_DEFINITIONS
    LV_CONF_INCLUDE_SIMPLE
)
