#include <Arduino.h>
#include "view_menu.h"
#include "map_view.h"
#include "radar_view.h"
#include "views.h"
#include "status_bar.h"
#include "location_picker.h"
#include "display_prefs.h"
#include "../pins_config.h"
#include "../data/storage.h"

#define PANEL_W 270
#define PANEL_H 360

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
        // Guaranteed flush -- a backstop for whatever the in-memory
        // g_config.trails_enabled/trail_max_points ended up as, regardless
        // of whether every individual widget-level save fired (this is
        // what fixed trail settings not surviving a reboot). Closing the
        // popover is a single discrete event, not a hot path, so an extra
        // write here is cheap.
        storage_save_config(g_config);

        // Same hide-then-delete-async pattern as location_picker.cpp -- this
        // can run from a click event on a descendant (every switch/slider/
        // button here lives under _overlay), and deleting an ancestor of a
        // still-dispatching event is undefined behavior in LVGL.
        lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(_overlay);
        _overlay = nullptr;
        _panel = nullptr;
        _len_label = nullptr;
    }
}

static void section_header(lv_obj_t *parent, const char *text, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, 0);
    lv_obj_set_pos(lbl, 0, y);
}

#define SW_W 44
#define SW_H 22
#define SW_KNOB_D 18
#define SW_PAD 2

// Moves the knob + recolors the track to look like an actual toggle switch
// (oval track, circular knob sliding side to side) -- same visual language
// as Settings' real lv_switch controls, even though this is built from
// plain lv_obj_create() objects, not lv_switch (see toggle_row() for why).
static void set_switch_state(lv_obj_t *track, bool on) {
    lv_obj_t *knob = lv_obj_get_child(track, 0);
    if (on) {
        lv_obj_set_style_bg_color(track, COLOR_ACCENT, 0);
        lv_obj_set_pos(knob, SW_W - SW_KNOB_D - SW_PAD, SW_PAD);
    } else {
        lv_obj_set_style_bg_color(track, lv_color_hex(0x333366), 0);
        lv_obj_set_pos(knob, SW_PAD, SW_PAD);
    }
}

// Shared row builder for every plain on/off toggle in this popover (tags,
// secondary locations, and trails' own on/off) -- label on the left, a
// switch-styled control on the right, same look/spacing throughout.
// Deliberately NOT an lv_switch: every other toggle in this app (GND
// button, filter buttons, nav tabs) is a plain clickable object +
// LV_EVENT_CLICKED -- lv_switch was the one place using
// LV_EVENT_VALUE_CHANGED instead, and was reported unusable (didn't
// respond to taps). The whole row is clickable, not just the switch
// track, for a bigger/easier hit target -- cb receives the track as its
// user_data so it can update it after toggling.
static lv_obj_t *toggle_row(lv_obj_t *parent, const char *label, int y,
                             bool initial, lv_event_cb_t cb) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, PANEL_W - 20, 30);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, COLOR_DIM, 0);
    lv_obj_set_pos(lbl, 0, 6);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *track = lv_obj_create(row);
    lv_obj_set_size(track, SW_W, SW_H);
    lv_obj_set_pos(track, PANEL_W - 20 - SW_W, 4);
    lv_obj_set_style_radius(track, SW_H / 2, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE); // row handles the tap, not the track

    lv_obj_t *knob = lv_obj_create(track);
    lv_obj_set_size(knob, SW_KNOB_D, SW_KNOB_D);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);

    set_switch_state(track, initial);

    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, track);
    return track;
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

    // Anchored under the VIEW chip itself (same idea as the location
    // picker's popover appearing under its own button) instead of a fixed
    // top-left position unrelated to where the chip actually is -- clamped
    // so it can't run off the right edge of the screen.
    int px = status_bar_get_view_chip_x();
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

    // No "VIEW" title here -- the chip that opens this already says VIEW,
    // right above it in the status bar.

    // ============================================================
    // Trails
    // ============================================================
    section_header(_panel, "TRAILS", 0);

    toggle_row(_panel, "Show trails", 26, g_config.trails_enabled, [](lv_event_t *e) {
        g_config.trails_enabled = !g_config.trails_enabled;
        storage_save_config(g_config);
        set_switch_state((lv_obj_t *)lv_event_get_user_data(e), g_config.trails_enabled);
    });

    // "Trail Amount" -- not "Length" or a "pts" count, since the effective
    // on-screen trail is scaled by the current view radius (see
    // map_view.cpp/radar_view.cpp) rather than a literal absolute point
    // count. "N/60" reads as a relative amount (out of the max
    // representable) instead of asserting a unit that isn't really true at
    // any zoom other than the widest radius preset.
    lv_obj_t *len_lbl = lv_label_create(_panel);
    lv_label_set_text(len_lbl, "Trail Amount");
    lv_obj_set_style_text_font(len_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(len_lbl, COLOR_DIM, 0);
    lv_obj_set_pos(len_lbl, 0, 62);

    _len_label = lv_label_create(_panel);
    lv_label_set_text_fmt(_len_label, "%d/60", g_config.trail_max_points);
    lv_obj_set_style_text_color(_len_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(_len_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_len_label, PANEL_W - 20 - 50, 62);

    lv_obj_t *slider = lv_slider_create(_panel);
    lv_obj_set_size(slider, PANEL_W - 20, 10);
    lv_obj_set_pos(slider, 0, 86);
    lv_slider_set_range(slider, 10, 60);
    lv_slider_set_value(slider, g_config.trail_max_points, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_KNOB);
    // VALUE_CHANGED fires repeatedly while dragging -- update the live value
    // and label on every tick (cheap, in-memory only), but only persist to
    // NVS on RELEASED/PRESS_LOST. storage_save_config() is a blocking flash
    // write; calling it on every drag tick stalls the LCD refresh badly
    // enough to cause a visible flash (see project_backlog memory).
    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        g_config.trail_max_points = val;
        lv_label_set_text_fmt(_len_label, "%d/60", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_RELEASED, nullptr);
    // A drag that ends with the finger slipping off the slider's bounds
    // (easy to do on a touchscreen) fires PRESS_LOST instead of RELEASED --
    // without this, that specific release pattern would skip the save
    // entirely (the popover-close backstop above only helps if the popover
    // is actually closed afterward, not if power is lost while it's still
    // open).
    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        storage_save_config(g_config);
    }, LV_EVENT_PRESS_LOST, nullptr);

    // Clear now -- dispatches to whichever of Map/Radar is currently active.
    lv_obj_t *clear_btn = lv_obj_create(_panel);
    lv_obj_set_size(clear_btn, PANEL_W - 20, 32);
    lv_obj_set_pos(clear_btn, 0, 104);
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

    // ============================================================
    // Tags -- each field independently toggleable. Flight ID falls back
    // callsign -> registration -> ICAO hex (never shows registration
    // alongside an existing callsign, per explicit feedback). Alt/Speed and
    // Type default off -- new capability on Map, stay minimal until turned
    // on (see storage.h).
    // ============================================================
    section_header(_panel, "TAGS", 160);
    toggle_row(_panel, "Flight ID", 188, tag_id_shown(), [](lv_event_t *e) {
        tag_id_toggle();
        set_switch_state((lv_obj_t *)lv_event_get_user_data(e), tag_id_shown());
    });
    toggle_row(_panel, "Alt / Speed", 222, tag_data_shown(), [](lv_event_t *e) {
        tag_data_toggle();
        set_switch_state((lv_obj_t *)lv_event_get_user_data(e), tag_data_shown());
    });
    toggle_row(_panel, "Type", 256, tag_type_shown(), [](lv_event_t *e) {
        tag_type_toggle();
        set_switch_state((lv_obj_t *)lv_event_get_user_data(e), tag_type_shown());
    });

    // ============================================================
    // Secondary locations -- other saved/static airports + the
    // HOME-elsewhere marker. Off gives the "just dots" look.
    // ============================================================
    section_header(_panel, "LOCATIONS", 290);
    toggle_row(_panel, "Other Airports", 318, secondary_locations_shown(), [](lv_event_t *e) {
        secondary_locations_toggle();
        set_switch_state((lv_obj_t *)lv_event_get_user_data(e), secondary_locations_shown());
    });
}

void view_menu_toggle() {
    if (_overlay) close_overlay();
    else open_overlay();
}

void view_menu_close() {
    close_overlay();
}
