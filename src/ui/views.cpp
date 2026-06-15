#include <Arduino.h>
#include "views.h"
#include "status_bar.h"
#include "map_view.h"
#include "radar_view.h"
#include "arrivals_view.h"
#include "stats_view.h"
#include "stats.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include "detail_card.h"
#include "alerts.h"
#include "range.h"

static lv_obj_t *tileview;
static lv_obj_t *tiles[4];
static int _active_index = VIEW_MAP;

// View cycle state — per-view dwell times (ms)
static const uint32_t CYCLE_DWELL_MS[] = {
    15000,  // MAP:      15s
    15000,  // RADAR:    15s
    15000,  // ARRIVALS: 15s
    5000,   // STATS:     5s (glance only during auto-cycle)
};
#define CYCLE_PAUSE_MS   60000  // 60s pause after touch
static uint32_t _last_touch_time = 0;
static uint32_t _last_cycle_time = 0;
static bool _cycle_paused = false;

#define STATUS_BAR_HEIGHT 30
#define CONTENT_Y STATUS_BAR_HEIGHT
#define CONTENT_H (LCD_V_RES - STATUS_BAR_HEIGHT)

static void tileview_changed_cb(lv_event_t *e) {
    lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < 4; i++) {
        if (tiles[i] == active) {
            _active_index = i;
            status_bar_set_active_dot(i);
            // Force immediate redraw of the newly active view
            lv_obj_invalidate(tiles[i]);
            break;
        }
    }
    // Dismiss overlays when switching views
    detail_card_hide();
    alerts_dismiss();
}

static void touch_pause_cb(lv_event_t *e) {
    _last_touch_time = millis();
    _last_cycle_time = _last_touch_time; // reset dwell timer so full 10s after unpause
    if (!_cycle_paused) {
        _cycle_paused = true;
        status_bar_set_auto_indicator(false);
    }
}

static void cycle_timer_cb(lv_timer_t *t) {
    // Respect config toggle
    if (!g_config.cycle_enabled) {
        if (!_cycle_paused) {
            _cycle_paused = true;
            status_bar_set_auto_indicator(false);
        }
        return;
    }

    uint32_t now = millis();
    uint32_t pause_ms = (uint32_t)g_config.cycle_inactivity_s * 1000;

    // Check if pause has expired
    if (_cycle_paused) {
        if (now - _last_touch_time >= pause_ms) {
            _cycle_paused = false;
            _last_cycle_time = now;
            status_bar_set_auto_indicator(true);
        }
        return;
    }

    // Use per-view dwell: config interval for MAP/RADAR/ARRIVALS, 5s for STATS
    uint32_t dwell = (_active_index == VIEW_STATS) ? 5000
                     : (uint32_t)g_config.cycle_interval_s * 1000;

    if (now - _last_cycle_time >= dwell) {
        _last_cycle_time = now;
        int next = (_active_index + 1) % 4;
        if (next == VIEW_MAP) range_cycle();
        // Update state directly — don't rely on VALUE_CHANGED callback
        _active_index = next;
        status_bar_set_active_dot(next);
        if (next != VIEW_ARRIVALS) lv_obj_invalidate(tiles[next]);
        lv_tileview_set_tile_by_index(tileview, next, 0, LV_ANIM_OFF);
        if (next == VIEW_ARRIVALS) arrivals_view_on_show();
    }
}

void views_init(lv_obj_t *parent, AircraftList *list) {
    stats_init();

    // Tileview fills screen below status bar
    tileview = lv_tileview_create(parent);
    lv_obj_set_pos(tileview, 0, CONTENT_Y);
    lv_obj_set_size(tileview, LCD_H_RES, CONTENT_H);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);

    // Create 4 horizontal tiles — all get opaque backgrounds to prevent bleed-through during scroll animation
    tiles[VIEW_MAP] = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    tiles[VIEW_RADAR] = lv_tileview_add_tile(tileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_ARRIVALS] = lv_tileview_add_tile(tileview, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_STATS] = lv_tileview_add_tile(tileview, 3, 0, LV_DIR_LEFT);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(tiles[i], lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(tiles[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tiles[i], 0, 0);
    }

    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Touch anywhere on tileview pauses auto-cycle
    lv_obj_add_event_cb(tileview, touch_pause_cb, LV_EVENT_PRESSED, nullptr);

    // Init all views
    map_view_init(tiles[VIEW_MAP], list);
    radar_view_init(tiles[VIEW_RADAR], list);
    arrivals_view_init(tiles[VIEW_ARRIVALS], list);
    stats_view_init(tiles[VIEW_STATS], list);

    // Start auto-cycle timer (respects g_config.cycle_enabled)
    _last_cycle_time = millis();
    status_bar_set_auto_indicator(g_config.cycle_enabled);
    if (!g_config.cycle_enabled) _cycle_paused = true;
    lv_timer_create(cycle_timer_cb, 1000, nullptr);
}

lv_obj_t *views_get_tile(int view_index) {
    return tiles[view_index];
}

int views_get_active_index() {
    return _active_index;
}

lv_obj_t *views_get_tileview() {
    return tileview;
}

void views_switch_to(int idx) {
    if (idx < 0 || idx > 3) return;
    _active_index = idx;
    status_bar_set_active_dot(idx);
    if (idx != VIEW_ARRIVALS) lv_obj_invalidate(tiles[idx]);
    lv_tileview_set_tile_by_index(tileview, idx, 0, LV_ANIM_OFF);
    if (idx == VIEW_ARRIVALS) arrivals_view_on_show();
    detail_card_hide();
    alerts_dismiss();
    views_pause_cycle();
}

static bool _swipe_active = false;
static lv_point_t _swipe_start = {0, 0};
#define SWIPE_THRESHOLD 50  // px horizontal travel to trigger view switch

bool views_swipe_active() { return _swipe_active; }
void views_clear_swipe()   { _swipe_active = false; }

void views_attach_swipe(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj, [](lv_event_t *e) {
        lv_indev_get_point(lv_indev_active(), &_swipe_start);
        _swipe_active = false;
    }, LV_EVENT_PRESSED, nullptr);

    lv_obj_add_event_cb(obj, [](lv_event_t *e) {
        if (_swipe_active) return;
        lv_point_t cur;
        lv_indev_get_point(lv_indev_active(), &cur);
        int dx = cur.x - _swipe_start.x;
        int dy = cur.y - _swipe_start.y;
        if (abs(dx) >= SWIPE_THRESHOLD && abs(dx) > abs(dy) * 2) {
            _swipe_active = true;
            int v = views_get_active_index();
            views_switch_to((dx < 0) ? (v + 1) % 4 : (v + 3) % 4);
        }
    }, LV_EVENT_PRESSING, nullptr);
}

void views_pause_cycle() {
    _last_touch_time = millis();
    _last_cycle_time = _last_touch_time;
    if (!_cycle_paused) {
        _cycle_paused = true;
        status_bar_set_auto_indicator(false);
    }
}
