#include <Arduino.h>
#include "trail_menu.h"
#include "map_view.h"
#include "radar_view.h"
#include "views.h"
#include "status_bar.h"
#include "location_picker.h"
#include "../pins_config.h"
#include "../data/storage.h"

#define PANEL_W 260
#define PANEL_H 180

#define COLOR_PANEL  lv_color_hex(0x14142a)
#define COLOR_ACCENT lv_color_hex(0x00cc66)
#define COLOR_TEXT   lv_color_hex(0xccccdd)
#define COLOR_DIM    lv_color_hex(0x888899)
#define COLOR_ROW    lv_color_hex(0x1a1a2e)

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_len_label = nullptr;

static void close_overlay() {
    if (_overlay) {
        // Same hide-then-delete-async pattern as location_picker.cpp -- this
        // can run from a click event on a descendant (the switch/slider/
        // clear button all live under _overlay), and deleting an ancestor of
        // a still-dispatching event is undefined behavior in LVGL.
        lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(_overlay);
        _overlay = nullptr;
        _panel = nullptr;
        _len_label = nullptr;
    }
}

static void open_overlay() {
    if (_overlay) return;
    location_picker_close(); // only one status-bar popover open at a time

    _overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_overlay, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT);
    lv_obj_set_pos(_overlay, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _overlay) close_overlay();
    }, LV_EVENT_CLICKED, nullptr);

    // Anchored under the TRAIL chip itself (same idea as the location
    // picker's popover appearing under its own button) instead of a fixed
    // top-left position unrelated to where the chip actually is -- clamped
    // so it can't run off the right edge of the screen.
    int px = status_bar_get_trails_chip_x();
    if (px + PANEL_W > LCD_H_RES - 8) px = LCD_H_RES - PANEL_W - 8;

    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_set_pos(_panel, px, 8);
    lv_obj_set_style_bg_color(_panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_border_color(_panel, COLOR_DIM, 0);
    lv_obj_set_style_border_opa(_panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(_panel, 8, 0);
    lv_obj_set_style_pad_all(_panel, 10, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, "TRAILS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_pos(title, 0, 0);

    // On/off
    lv_obj_t *on_lbl = lv_label_create(_panel);
    lv_label_set_text(on_lbl, "Show trails");
    lv_obj_set_style_text_font(on_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(on_lbl, COLOR_DIM, 0);
    lv_obj_set_pos(on_lbl, 0, 32);

    lv_obj_t *sw = lv_switch_create(_panel);
    lv_obj_set_pos(sw, PANEL_W - 20 - 46, 26);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(sw, COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (g_config.trails_enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
    // Same instant-apply-and-save pattern as the GND filter button
    // (map_view.cpp etc.) -- no separate Save step for a quick-settings popover.
    lv_obj_add_event_cb(sw, [](lv_event_t *e) {
        g_config.trails_enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
        storage_save_config(g_config);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Length
    lv_obj_t *len_lbl = lv_label_create(_panel);
    lv_label_set_text(len_lbl, "Length");
    lv_obj_set_style_text_font(len_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(len_lbl, COLOR_DIM, 0);
    lv_obj_set_pos(len_lbl, 0, 66);

    _len_label = lv_label_create(_panel);
    lv_label_set_text_fmt(_len_label, "%d pts", g_config.trail_max_points);
    lv_obj_set_style_text_color(_len_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(_len_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_len_label, PANEL_W - 20 - 50, 66);

    lv_obj_t *slider = lv_slider_create(_panel);
    lv_obj_set_size(slider, PANEL_W - 20, 10);
    lv_obj_set_pos(slider, 0, 90);
    lv_slider_set_range(slider, 10, 60);
    lv_slider_set_value(slider, g_config.trail_max_points, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        g_config.trail_max_points = val;
        lv_label_set_text_fmt(_len_label, "%d pts", val);
        storage_save_config(g_config);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Clear now -- dispatches to whichever of Map/Radar is currently active,
    // same as the chip's old direct-clear behavior.
    lv_obj_t *clear_btn = lv_obj_create(_panel);
    lv_obj_set_size(clear_btn, PANEL_W - 20, 32);
    lv_obj_set_pos(clear_btn, 0, 116);
    lv_obj_set_style_bg_color(clear_btn, COLOR_ROW, 0);
    lv_obj_set_style_border_color(clear_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(clear_btn, 1, 0);
    lv_obj_set_style_radius(clear_btn, 6, 0);
    lv_obj_set_style_pad_all(clear_btn, 0, 0);
    lv_obj_clear_flag(clear_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clear_btn, [](lv_event_t *e) {
        int v = views_get_active_index();
        if (v == VIEW_MAP) map_view_clear_trails();
        else if (v == VIEW_RADAR) radar_view_clear_trails();
        // Close so the (otherwise dimmed-by-the-overlay) map/radar canvas is
        // immediately visible again -- the clearest confirmation that this
        // actually did something.
        close_overlay();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear Now");
    lv_obj_set_style_text_color(clear_lbl, COLOR_ACCENT, 0);
    lv_obj_center(clear_lbl);
}

void trail_menu_toggle() {
    if (_overlay) close_overlay();
    else open_overlay();
}

void trail_menu_close() {
    close_overlay();
}
