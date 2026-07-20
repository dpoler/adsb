#pragma once
#include "lvgl.h"

enum AlertType {
    ALERT_MILITARY,
    ALERT_EMERGENCY,
    ALERT_WATCHLIST,
};

// Initialize alert system (call once from LVGL context)
void alerts_init(lv_obj_t *parent);

// Show a toast notification immediately (LVGL context only)
void alerts_show(AlertType type, const char *title, const char *detail,
                 const char *icao_hex = nullptr, uint32_t timeout_ms = 10000);

// Queue an alert from any task (thread-safe). Displayed on next LVGL tick.
void alerts_queue(AlertType type, const char *title, const char *detail,
                  const char *icao_hex = nullptr);

// Dismiss any visible toast immediately
void alerts_dismiss();
