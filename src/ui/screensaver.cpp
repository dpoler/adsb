#include <Arduino.h>
#include "screensaver.h"
#include "status_bar.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include <cstdlib>

// ============================================================
// DEACTIVATED (2026-07-23) -- kept in place for a later redesign, not
// deleted. This board's JD9165 panel is a MIPI-DSI TFT LCD (see
// hal/jd9165_lcd.cpp), not OLED -- the burn-in-avoidance motivation behind
// the moving-text screensaver below doesn't actually apply to this
// hardware (TFT LCDs have no per-pixel emissive aging the way OLED does;
// the closest LCD analog, temporary "image persistence," needs weeks of a
// static image and fades on its own). Brightness/dim/blank may still be
// worth keeping without the screensaver motion, but the whole feature is
// being rethought, so everything below is disabled rather than picked
// apart. The two public entry points at the bottom of this file are now
// no-ops, so main.cpp doesn't need to change at all.
// ============================================================
#if 0

#define COLOR_PANEL  lv_color_hex(0x14142a)
#define COLOR_ACCENT lv_color_hex(0x00cc66)
#define COLOR_TEXT   lv_color_hex(0xccccdd)
#define COLOR_DIM    lv_color_hex(0x888899)

#define SETTINGS_PANEL_W 320
#define SETTINGS_PANEL_H 300

#define SS_BOX_W 260
#define SS_BOX_H 90

static backlight_set_fn _set_backlight = nullptr;

// The screensaver's own full-screen overlay -- distinct from the settings
// popover below, which only exists to configure this behavior, not to
// display it.
static lv_obj_t *_ss_overlay = nullptr;
static lv_obj_t *_ss_box = nullptr;
static lv_obj_t *_ss_count_label = nullptr;

enum DispState { DISP_NORMAL, DISP_DIM, DISP_BLANK };
static DispState _state = DISP_NORMAL;
static int _ss_dx = 2, _ss_dy = 2;      // drift velocity, px/tick
static uint32_t _last_jump_ms = 0;

// Dim level is a fraction of the user's set brightness, not independently
// configurable -- floored so it's never literally imperceptible (that's
// what the separate blank state is for).
static int dim_level(int brightness_pct) {
    int lvl = (brightness_pct * 25) / 100;
    return lvl < 5 ? 5 : lvl;
}

static void position_ss_box_randomly() {
    int max_x = LCD_H_RES - SS_BOX_W;
    int max_y = LCD_V_RES - SS_BOX_H;
    lv_obj_set_pos(_ss_box, rand() % (max_x > 0 ? max_x : 1), rand() % (max_y > 0 ? max_y : 1));
}

static void enter_state(DispState s) {
    _state = s;
    switch (s) {
    case DISP_NORMAL:
        if (_set_backlight) _set_backlight(g_config.display_brightness_pct);
        lv_obj_add_flag(_ss_overlay, LV_OBJ_FLAG_HIDDEN);
        break;
    case DISP_DIM:
        if (_set_backlight) _set_backlight(dim_level(g_config.display_brightness_pct));
        lv_obj_add_flag(_ss_overlay, LV_OBJ_FLAG_HIDDEN);
        break;
    case DISP_BLANK:
        if (g_config.screensaver_enabled) {
            // Backlight is left exactly as it was (full or already-dimmed) --
            // the point is showing the moving count, not a level change.
            lv_label_set_text_fmt(_ss_count_label, "%d", status_bar_get_aircraft_count());
            position_ss_box_randomly();
            _last_jump_ms = millis();
            _ss_dx = (rand() % 2) ? 2 : -2;
            _ss_dy = (rand() % 2) ? 2 : -2;
            lv_obj_clear_flag(_ss_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (_set_backlight) _set_backlight(0);
            lv_obj_add_flag(_ss_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    }
}

static void state_timer_cb(lv_timer_t *t) {
    uint32_t idle_ms = lv_display_get_inactive_time(nullptr);
    uint32_t dim_ms = (uint32_t)g_config.display_dim_after_min * 60000UL;
    uint32_t blank_ms = (uint32_t)g_config.display_blank_after_min * 60000UL;

    DispState desired = DISP_NORMAL;
    if (blank_ms > 0 && idle_ms >= blank_ms) desired = DISP_BLANK;
    else if (dim_ms > 0 && idle_ms >= dim_ms) desired = DISP_DIM;

    if (desired != _state) enter_state(desired);

    if (_state != DISP_BLANK || !g_config.screensaver_enabled) return;

    // Keep the count fresh while it's on screen, and move it around so a
    // fixed-position display never sits static for the whole idle period.
    lv_label_set_text_fmt(_ss_count_label, "%d", status_bar_get_aircraft_count());

    if (g_config.screensaver_drift) {
        int x = lv_obj_get_x(_ss_box) + _ss_dx;
        int y = lv_obj_get_y(_ss_box) + _ss_dy;
        int max_x = LCD_H_RES - SS_BOX_W;
        int max_y = LCD_V_RES - SS_BOX_H;
        if (x <= 0 || x >= max_x) _ss_dx = -_ss_dx;
        if (y <= 0 || y >= max_y) _ss_dy = -_ss_dy;
        if (x < 0) x = 0; if (x > max_x) x = max_x;
        if (y < 0) y = 0; if (y > max_y) y = max_y;
        lv_obj_set_pos(_ss_box, x, y);
    } else {
        uint32_t now = millis();
        if (now - _last_jump_ms >= 20000) {
            _last_jump_ms = now;
            position_ss_box_randomly();
        }
    }
}

static void build_display_overlay(lv_obj_t *screen) {
    _ss_overlay = lv_obj_create(screen);
    lv_obj_set_size(_ss_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_ss_overlay, 0, 0);
    lv_obj_set_style_bg_color(_ss_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_ss_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_ss_overlay, 0, 0);
    lv_obj_set_style_radius(_ss_overlay, 0, 0);
    lv_obj_clear_flag(_ss_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_ss_overlay, LV_OBJ_FLAG_HIDDEN);

    _ss_box = lv_obj_create(_ss_overlay);
    lv_obj_set_size(_ss_box, SS_BOX_W, SS_BOX_H);
    lv_obj_set_pos(_ss_box, 0, 0);
    lv_obj_set_style_bg_opa(_ss_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_ss_box, 0, 0);
    lv_obj_clear_flag(_ss_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_ss_box, LV_OBJ_FLAG_CLICKABLE);

    _ss_count_label = lv_label_create(_ss_box);
    lv_label_set_text(_ss_count_label, "0");
    lv_obj_set_style_text_color(_ss_count_label, lv_color_hex(0x336644), 0);
    lv_obj_set_style_text_font(_ss_count_label, &lv_font_montserrat_28, 0);
    lv_obj_align(_ss_count_label, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *caption = lv_label_create(_ss_box);
    lv_label_set_text(caption, "AIRCRAFT TRACKED");
    lv_obj_set_style_text_color(caption, lv_color_hex(0x224433), 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Move to the very top so it draws over the status bar, whichever view
    // is active, and any popover left open when the user stepped away --
    // safer than relying on creation order, since screensaver_init() runs
    // near the end of setup() but other overlays can still be built/rebuilt
    // after that.
    lv_obj_move_foreground(_ss_overlay);
}

// ============================================================
// Settings popover -- opened from a button in settings.cpp. Every control
// here applies and persists immediately on change (same instant-apply
// convention as the VIEW menu/filters/GND, not the main Settings panel's
// apply-on-Save), since there's no separate "close" step that would make
// deferred writes safe the way the popover-close backstop does elsewhere.
// ============================================================

static lv_obj_t *_cfg_overlay = nullptr;

static void close_settings() {
    if (_cfg_overlay) {
        lv_obj_add_flag(_cfg_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(_cfg_overlay);
        _cfg_overlay = nullptr;
    }
}

static lv_obj_t *cfg_slider_row(lv_obj_t *parent, const char *label, int y,
                                 int min, int max, int initial,
                                 lv_obj_t **out_value_label) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, COLOR_DIM, 0);
    lv_obj_set_pos(lbl, 0, y);

    lv_obj_t *val = lv_label_create(parent);
    lv_obj_set_style_text_color(val, lv_color_white(), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(val, SETTINGS_PANEL_W - 20 - 60, y);
    *out_value_label = val;

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, SETTINGS_PANEL_W - 20, 10);
    lv_obj_set_pos(slider, 0, y + 22);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_KNOB);
    return slider;
}

void screensaver_show_settings() {
    if (_cfg_overlay) return;

    _cfg_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_cfg_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_cfg_overlay, 0, 0);
    lv_obj_set_style_bg_color(_cfg_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_cfg_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_cfg_overlay, 0, 0);
    lv_obj_set_style_radius(_cfg_overlay, 0, 0);
    lv_obj_clear_flag(_cfg_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_cfg_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _cfg_overlay) close_settings();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *panel = lv_obj_create(_cfg_overlay);
    lv_obj_set_size(panel, SETTINGS_PANEL_W, SETTINGS_PANEL_H);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COLOR_DIM, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Display & Screensaver");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 0, 0);

    // --- Brightness -- applies live so dragging previews the level directly.
    lv_obj_t *bright_val;
    lv_obj_t *bright_slider = cfg_slider_row(panel, "Brightness", 32, 10, 100,
                                              g_config.display_brightness_pct, &bright_val);
    lv_label_set_text_fmt(bright_val, "%d%%", g_config.display_brightness_pct);
    lv_obj_add_event_cb(bright_slider, [](lv_event_t *e) {
        lv_obj_t *val = (lv_obj_t *)lv_event_get_user_data(e);
        int v = lv_slider_get_value(lv_event_get_target_obj(e));
        g_config.display_brightness_pct = v;
        if (_set_backlight) _set_backlight(v);
        lv_label_set_text_fmt(val, "%d%%", v);
    }, LV_EVENT_VALUE_CHANGED, bright_val);
    lv_obj_add_event_cb(bright_slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(bright_slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_PRESS_LOST, nullptr);

    // --- Dim After (minutes, 0 = never)
    lv_obj_t *dim_val;
    lv_obj_t *dim_slider = cfg_slider_row(panel, "Dim After", 84, 0, 60,
                                           g_config.display_dim_after_min, &dim_val);
    lv_label_set_text_fmt(dim_val, g_config.display_dim_after_min == 0 ? "Off" : "%d min", g_config.display_dim_after_min);
    lv_obj_add_event_cb(dim_slider, [](lv_event_t *e) {
        lv_obj_t *val = (lv_obj_t *)lv_event_get_user_data(e);
        int v = lv_slider_get_value(lv_event_get_target_obj(e));
        g_config.display_dim_after_min = v;
        if (v == 0) lv_label_set_text(val, "Off");
        else lv_label_set_text_fmt(val, "%d min", v);
    }, LV_EVENT_VALUE_CHANGED, dim_val);
    lv_obj_add_event_cb(dim_slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(dim_slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_PRESS_LOST, nullptr);

    // --- Blank After (minutes, 0 = never)
    lv_obj_t *blank_val;
    lv_obj_t *blank_slider = cfg_slider_row(panel, "Blank After", 136, 0, 60,
                                             g_config.display_blank_after_min, &blank_val);
    lv_label_set_text_fmt(blank_val, g_config.display_blank_after_min == 0 ? "Off" : "%d min", g_config.display_blank_after_min);
    lv_obj_add_event_cb(blank_slider, [](lv_event_t *e) {
        lv_obj_t *val = (lv_obj_t *)lv_event_get_user_data(e);
        int v = lv_slider_get_value(lv_event_get_target_obj(e));
        g_config.display_blank_after_min = v;
        if (v == 0) lv_label_set_text(val, "Off");
        else lv_label_set_text_fmt(val, "%d min", v);
    }, LV_EVENT_VALUE_CHANGED, blank_val);
    lv_obj_add_event_cb(blank_slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(blank_slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_PRESS_LOST, nullptr);

    // --- Screensaver on/off -- shows a moving aircraft count instead of a
    // plain backlight-off blank once Blank After elapses.
    lv_obj_t *ss_lbl = lv_label_create(panel);
    lv_label_set_text(ss_lbl, "Screensaver");
    lv_obj_set_style_text_font(ss_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ss_lbl, COLOR_DIM, 0);
    lv_obj_set_pos(ss_lbl, 0, 192);

    lv_obj_t *ss_sw = lv_switch_create(panel);
    lv_obj_set_pos(ss_sw, SETTINGS_PANEL_W - 20 - 46, 188);
    lv_obj_set_style_bg_color(ss_sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(ss_sw, COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (g_config.screensaver_enabled) lv_obj_add_state(ss_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ss_sw, [](lv_event_t *e) {
        g_config.screensaver_enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
        storage_save_config(g_config);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- Drift continuously vs. jump every 20s
    lv_obj_t *mv_lbl = lv_label_create(panel);
    lv_label_set_text(mv_lbl, "Continuous Drift");
    lv_obj_set_style_text_font(mv_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mv_lbl, COLOR_DIM, 0);
    lv_obj_set_pos(mv_lbl, 0, 226);

    lv_obj_t *mv_sw = lv_switch_create(panel);
    lv_obj_set_pos(mv_sw, SETTINGS_PANEL_W - 20 - 46, 222);
    lv_obj_set_style_bg_color(mv_sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(mv_sw, COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (g_config.screensaver_drift) lv_obj_add_state(mv_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(mv_sw, [](lv_event_t *e) {
        g_config.screensaver_drift = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
        storage_save_config(g_config);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
}

#endif // DEACTIVATED

void screensaver_init(lv_obj_t *, backlight_set_fn) {
    // Deactivated -- see the #if 0 block above.
}

void screensaver_show_settings() {
    // Deactivated -- see the #if 0 block above.
}
