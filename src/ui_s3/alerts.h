#pragma once
#include "lvgl.h"

enum AlertType {
    ALERT_MILITARY,
    ALERT_EMERGENCY,
    ALERT_WATCHLIST,
};

void alerts_init(lv_obj_t *parent);
void alerts_show(AlertType type, const char *title, const char *detail,
                 const char *icao_hex = nullptr, uint32_t timeout_ms = 10000);
void alerts_queue(AlertType type, const char *title, const char *detail,
                  const char *icao_hex = nullptr);
