#include "alerts.h"
#include "detail_card.h"
#include "views.h"
#include "map_view.h"
#include "../data/aircraft.h"
#include "../data/storage.h"
#include "../pins_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

// Aircraft list reference for tap-to-detail
extern AircraftList aircraft_list;

static lv_obj_t *_toast = nullptr;
static lv_obj_t *_toast_title = nullptr;
static lv_obj_t *_toast_detail = nullptr;
static lv_obj_t *_toast_icon = nullptr;
static lv_timer_t *_dismiss_timer = nullptr;

// ICAO hex of the currently displayed toast (for tap-to-detail lookup)
static char _current_hex[7] = {};
static bool _toast_active = false; // true while a toast is showing/waiting to auto-dismiss

#define TOAST_W 500
#define TOAST_H 50
#define TOAST_Y 35  // just below status bar

// --- Thread-safe alert queue ---
struct PendingAlert {
    AlertType type;
    char title[16];
    char detail[48];
    char icao_hex[7];
};

#define ALERT_QUEUE_SIZE 8
static PendingAlert _queue[ALERT_QUEUE_SIZE];
static volatile int _queue_head = 0;
static volatile int _queue_tail = 0;
static SemaphoreHandle_t _queue_mutex = nullptr;

static lv_color_t alert_color(AlertType type) {
    switch (type) {
        case ALERT_EMERGENCY:  return lv_color_hex(0xff2222);
        case ALERT_MILITARY:   return lv_color_hex(0xffaa00);
        case ALERT_WATCHLIST:  return lv_color_hex(0x4488ff);
    }
    return lv_color_white();
}

static const char *alert_icon(AlertType type) {
    switch (type) {
        case ALERT_EMERGENCY:  return LV_SYMBOL_WARNING;
        case ALERT_MILITARY:   return LV_SYMBOL_GPS;
        case ALERT_WATCHLIST:  return LV_SYMBOL_EYE_OPEN;
    }
    return LV_SYMBOL_BELL;
}

static void dismiss_toast(lv_timer_t *t) {
    _toast_active = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _toast);
    lv_anim_set_values(&a, lv_obj_get_y(_toast), -TOAST_H);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_y((lv_obj_t *)obj, v);
    });
    lv_anim_start(&a);

    if (_dismiss_timer) {
        lv_timer_delete(_dismiss_timer);
        _dismiss_timer = nullptr;
    }
}

// Drain queue from LVGL timer context -- at most one alert per tick, and
// only once the current toast has finished. Draining the whole queue in one
// synchronous burst (the old behavior) called alerts_show() repeatedly with
// no render in between, so every queued alert except the last got silently
// overwritten before it was ever drawn -- e.g. three military aircraft
// already in view on the first fetch would only ever show the last one.
static void process_queue(lv_timer_t *t) {
    if (_toast_active) return;
    if (xSemaphoreTake(_queue_mutex, 0) != pdTRUE) return;

    if (_queue_head != _queue_tail) {
        PendingAlert pa = _queue[_queue_tail]; // copy out before releasing the mutex
        _queue_tail = (_queue_tail + 1) % ALERT_QUEUE_SIZE;
        xSemaphoreGive(_queue_mutex);
        alerts_show(pa.type, pa.title, pa.detail, pa.icao_hex);
        return;
    }

    xSemaphoreGive(_queue_mutex);
}

void alerts_init(lv_obj_t *parent) {
    _queue_mutex = xSemaphoreCreateMutex();

    _toast = lv_obj_create(parent);
    lv_obj_set_size(_toast, TOAST_W, TOAST_H);
    lv_obj_set_pos(_toast, (LCD_H_RES - TOAST_W) / 2, -TOAST_H); // hidden above
    lv_obj_set_style_bg_color(_toast, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(_toast, 8, 0);
    lv_obj_set_style_border_width(_toast, 2, 0);
    lv_obj_set_style_border_color(_toast, lv_color_white(), 0);
    lv_obj_set_style_pad_all(_toast, 8, 0);
    lv_obj_clear_flag(_toast, LV_OBJ_FLAG_SCROLLABLE);

    _toast_icon = lv_label_create(_toast);
    lv_obj_set_style_text_font(_toast_icon, &lv_font_montserrat_20, 0);
    lv_obj_align(_toast_icon, LV_ALIGN_LEFT_MID, 0, 0);

    _toast_title = lv_label_create(_toast);
    lv_obj_set_style_text_font(_toast_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_toast_title, lv_color_white(), 0);
    lv_obj_align(_toast_title, LV_ALIGN_LEFT_MID, 32, -8);

    _toast_detail = lv_label_create(_toast);
    lv_obj_set_style_text_font(_toast_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_toast_detail, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(_toast_detail, LV_ALIGN_LEFT_MID, 32, 10);

    // Tap: switch to map, track aircraft, show detail card, dismiss
    lv_obj_add_event_cb(_toast, [](lv_event_t *e) {
        if (_current_hex[0] && aircraft_list.lock(pdMS_TO_TICKS(10))) {
            for (int i = 0; i < aircraft_list.count; i++) {
                if (strcmp(aircraft_list.aircraft[i].icao_hex, _current_hex) == 0) {
                    Aircraft ac_copy = aircraft_list.aircraft[i];
                    aircraft_list.unlock();
                    views_pause_cycle();
                    lv_tileview_set_tile_by_index(views_get_tileview(), VIEW_MAP, 0, LV_ANIM_OFF);
                    map_view_track(ac_copy.icao_hex);
                    detail_card_show(&ac_copy);
                    dismiss_toast(nullptr);
                    return;
                }
            }
            aircraft_list.unlock();
        }
        dismiss_toast(nullptr);
    }, LV_EVENT_CLICKED, nullptr);

    // Periodic queue drain (every 500ms)
    lv_timer_create(process_queue, 500, nullptr);
}

void alerts_show(AlertType type, const char *title, const char *detail,
                 const char *icao_hex, uint32_t timeout_ms) {
    _toast_active = true;
    lv_color_t color = alert_color(type);

    // Store hex for tap-to-detail
    if (icao_hex && icao_hex[0]) {
        strlcpy(_current_hex, icao_hex, sizeof(_current_hex));
    } else {
        _current_hex[0] = '\0';
    }

    // Military/emergency alerts get a longer 10s timeout. No auto-switch to
    // Map -- popping up the toast is enough; changing what the user is
    // looking at was more disruptive than helpful.
    if (type == ALERT_MILITARY || type == ALERT_EMERGENCY) {
        timeout_ms = 10000;
    }

    lv_obj_set_style_border_color(_toast, color, 0);
    lv_label_set_text(_toast_icon, alert_icon(type));
    lv_obj_set_style_text_color(_toast_icon, color, 0);
    lv_label_set_text(_toast_title, title);
    lv_label_set_text(_toast_detail, detail);

    // Slide in from top
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _toast);
    lv_anim_set_values(&a, -TOAST_H, TOAST_Y);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_y((lv_obj_t *)obj, v);
    });
    lv_anim_start(&a);

    // Auto-dismiss timer
    if (_dismiss_timer) lv_timer_delete(_dismiss_timer);
    _dismiss_timer = lv_timer_create(dismiss_toast, timeout_ms, nullptr);
    lv_timer_set_repeat_count(_dismiss_timer, 1);
}

void alerts_dismiss() {
    dismiss_toast(nullptr);
}

void alerts_queue(AlertType type, const char *title, const char *detail,
                  const char *icao_hex) {
    if (!_queue_mutex) return;
    if (xSemaphoreTake(_queue_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    int next = (_queue_head + 1) % ALERT_QUEUE_SIZE;
    if (next != _queue_tail) { // not full
        PendingAlert &pa = _queue[_queue_head];
        pa.type = type;
        strncpy(pa.title, title, sizeof(pa.title) - 1);
        pa.title[sizeof(pa.title) - 1] = '\0';
        strncpy(pa.detail, detail, sizeof(pa.detail) - 1);
        pa.detail[sizeof(pa.detail) - 1] = '\0';
        if (icao_hex && icao_hex[0]) {
            strlcpy(pa.icao_hex, icao_hex, sizeof(pa.icao_hex));
        } else {
            pa.icao_hex[0] = '\0';
        }
        _queue_head = next;
    }

    xSemaphoreGive(_queue_mutex);
}
