#include "location_picker.h"
#include "../data/locations.h"
#include "../data/storage.h"
#include "../pins_config.h"
#include "status_bar.h"
#include <Arduino.h>
#include <cstring>
#include <cctype>

#define PANEL_W    320
#define ROW_H      44
#define BTN_W      60   // matches status_bar.cpp's CHIP_W -- same width as every other button in the bar (nav tabs, range/TRAIL/TAG chips)
#define BTN_H      24   // matches status_bar.cpp's CHIP_H (and the nav tabs' own height)

#define COLOR_BG        lv_color_hex(0x0d0d1a)
#define COLOR_PANEL     lv_color_hex(0x14142a)
#define COLOR_ROW_HOME  lv_color_hex(0x1a2a3a)
#define COLOR_ROW       lv_color_hex(0x1a1a2e)
#define COLOR_ACCENT    lv_color_hex(0x00cc66)
#define COLOR_TEXT      lv_color_hex(0xccccdd)
#define COLOR_DIM       lv_color_hex(0x888899)
#define COLOR_ERR       lv_color_hex(0xff6666)

static lv_obj_t *_picker_btn = nullptr;
static lv_obj_t *_picker_lbl = nullptr;
static lv_obj_t *_overlay = nullptr;   // full-screen dim backdrop, closes on tap
static lv_obj_t *_panel = nullptr;     // the actual popover content
static lv_obj_t *_keyboard = nullptr;  // shared by the add-view textarea, lives on _overlay

// The actual fetch runs on location_poll_task's existing stack (see
// locations_add_poll() / project_p4_heap_constraints memory — a dedicated
// task for this crashed the SDIO driver under memory pressure). This module
// just tracks whether a request is outstanding so the UI can poll for it.
static bool _add_in_progress = false;

static lv_obj_t *_add_status_lbl = nullptr;
static lv_obj_t *_add_fetch_btn = nullptr;
static lv_obj_t *_add_ta = nullptr;

static void build_list_view();
static void build_add_view();

static void update_picker_label() {
    int active = locations_active_index();
    if (active == -1) {
        lv_label_set_text(_picker_lbl, "HOME");
    } else {
        const Location *loc = locations_get(active);
        lv_label_set_text(_picker_lbl, loc ? loc->icao : "?");
    }
}

static void close_overlay() {
    if (_overlay) {
        // Deferred, not lv_obj_delete(): this often runs from a CLICKED event
        // on one of _overlay's own descendants (e.g. select_location() via
        // add_row_click_cb) -- deleting an ancestor of the widget still
        // dispatching its own event is undefined behavior in LVGL and was the
        // root cause of touch going unresponsive / changes seeming to need an
        // extra refresh to "take". Hide immediately so nothing lingers
        // visually; the actual delete happens safely on the next tick.
        lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(_overlay); // deletes _panel and _keyboard too (children)
        _overlay = nullptr;
        _panel = nullptr;
        _keyboard = nullptr;
        _add_status_lbl = nullptr;
        _add_fetch_btn = nullptr;
        _add_ta = nullptr;
    }
}

static void select_location(int idx) {
    locations_set_active(idx);
    update_picker_label();
    close_overlay();
}

static void open_overlay() {
    if (_overlay) return;

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

    // Shared on-screen keyboard for the "add airport" ICAO field — created
    // hidden, shown/hidden by build_add_view()'s textarea events.
    _keyboard = lv_keyboard_create(_overlay);
    lv_obj_set_size(_keyboard, LCD_H_RES, 200);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, [](lv_event_t *e) {
        lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_keyboard, [](lv_event_t *e) {
        lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CANCEL, nullptr);

    build_list_view();
}

static void picker_btn_click_cb(lv_event_t *e) {
    if (_overlay) close_overlay();
    else open_overlay();
}

static void add_row_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    select_location(idx);
}

static void remove_row_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    locations_remove(idx);
    update_picker_label(); // reflect a reset-to-Home immediately if the active airport was removed
    build_list_view(); // rebuild panel in place
}

// Note: build_list_view()/build_add_view() are also called from click events
// on a widget that is a *descendant* of _panel (e.g. remove_row_click_cb, the
// "Add airport" row, the add-view's "Cancel" button) -- deleting _panel
// synchronously in that case is undefined behavior in LVGL (the click event
// is still dispatching on a now-freed object) and previously manifested as
// touch becoming unresponsive after removing a saved airport. Hide + defer
// instead of an immediate lv_obj_delete().
static void build_list_view() {
    if (_panel) {
        lv_obj_add_flag(_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(_panel);
    }
    if (_keyboard) lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    // These only exist while _panel is the "add airport" view -- clear them so
    // a fetch result that arrives after Cancel (tapped while a fetch was still
    // in flight) doesn't write into the now-deleted widgets.
    _add_status_lbl = nullptr;
    _add_fetch_btn = nullptr;
    _add_ta = nullptr;

    int rows = 1 + locations_count() + 1; // Home + saved + "Add" row
    int panel_h = rows * ROW_H + 8;
    int max_h = LCD_V_RES - STATUS_BAR_HEIGHT - 16;
    if (panel_h > max_h) panel_h = max_h;

    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, panel_h);
    lv_obj_set_pos(_panel, 8, 8);
    lv_obj_set_style_bg_color(_panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_border_color(_panel, COLOR_DIM, 0);
    lv_obj_set_style_border_opa(_panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(_panel, 8, 0);
    lv_obj_set_style_pad_all(_panel, 4, 0);
    lv_obj_set_flex_flow(_panel, LV_FLEX_FLOW_COLUMN);

    // Home row
    {
        lv_obj_t *row = lv_obj_create(_panel);
        lv_obj_set_size(row, LV_PCT(100), ROW_H - 4);
        lv_obj_set_style_bg_color(row, COLOR_ROW_HOME, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, locations_active_index() == -1 ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, COLOR_ACCENT, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, add_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "HOME");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    // Saved airports
    int n = locations_count();
    for (int i = 0; i < n; i++) {
        const Location *loc = locations_get(i);
        if (!loc) continue;

        lv_obj_t *row = lv_obj_create(_panel);
        lv_obj_set_size(row, LV_PCT(100), ROW_H - 4);
        lv_obj_set_style_bg_color(row, COLOR_ROW, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, locations_active_index() == i ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, COLOR_ACCENT, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, add_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, loc->icao);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *rm = lv_label_create(row);
        lv_label_set_text(rm, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(rm, COLOR_DIM, 0);
        lv_obj_align(rm, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_add_flag(rm, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(rm, 10);
        lv_obj_add_event_cb(rm, remove_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // Add row
    {
        lv_obj_t *row = lv_obj_create(_panel);
        lv_obj_set_size(row, LV_PCT(100), ROW_H - 4);
        lv_obj_set_style_bg_color(row, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, COLOR_DIM, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, [](lv_event_t *e) { build_add_view(); }, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, LV_SYMBOL_PLUS "  Add airport");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, COLOR_ACCENT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void fetch_btn_click_cb(lv_event_t *e) {
    if (_add_in_progress) return;
    const char *text = lv_textarea_get_text(_add_ta);
    if (!text || strlen(text) < 3) {
        lv_label_set_text(_add_status_lbl, "Enter a 3-4 letter ICAO code");
        lv_obj_set_style_text_color(_add_status_lbl, COLOR_ERR, 0);
        return;
    }

    char icao[LOC_ICAO_LEN];
    strlcpy(icao, text, sizeof(icao));
    for (char *c = icao; *c; c++) *c = toupper((unsigned char)*c);

    _add_in_progress = true;
    lv_label_set_text(_add_status_lbl, "Fetching...");
    lv_obj_set_style_text_color(_add_status_lbl, COLOR_DIM, 0);

    locations_request_add(icao); // picked up by location_poll_task's loop
}

static void build_add_view() {
    if (_panel) {
        lv_obj_add_flag(_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(_panel); // see build_list_view() -- called from a click event on a descendant of _panel (the "Add airport" row)
    }

    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, 200);
    lv_obj_set_pos(_panel, 8, 8);
    lv_obj_set_style_bg_color(_panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_border_color(_panel, COLOR_DIM, 0);
    lv_obj_set_style_border_opa(_panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(_panel, 8, 0);
    lv_obj_set_style_pad_all(_panel, 10, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, "Add airport by ICAO");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_pos(title, 0, 0);

    _add_ta = lv_textarea_create(_panel);
    lv_obj_set_size(_add_ta, PANEL_W - 20, 40);
    lv_obj_set_pos(_add_ta, 0, 28);
    lv_textarea_set_one_line(_add_ta, true);
    lv_textarea_set_max_length(_add_ta, LOC_ICAO_LEN - 1);
    lv_textarea_set_placeholder_text(_add_ta, "e.g. KLGA");

    _add_fetch_btn = lv_obj_create(_panel);
    lv_obj_set_size(_add_fetch_btn, 90, BTN_H + 10);
    lv_obj_set_pos(_add_fetch_btn, 0, 76);
    lv_obj_set_style_bg_color(_add_fetch_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(_add_fetch_btn, 6, 0);
    lv_obj_clear_flag(_add_fetch_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_add_fetch_btn, fetch_btn_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *fetch_lbl = lv_label_create(_add_fetch_btn);
    lv_label_set_text(fetch_lbl, "Fetch");
    lv_obj_set_style_text_color(fetch_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(fetch_lbl);

    lv_obj_t *back_btn = lv_obj_create(_panel);
    lv_obj_set_size(back_btn, 90, BTN_H + 10);
    lv_obj_set_pos(back_btn, 100, 76);
    lv_obj_set_style_bg_color(back_btn, COLOR_ROW, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) { build_list_view(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Cancel");
    lv_obj_set_style_text_color(back_lbl, COLOR_DIM, 0);
    lv_obj_center(back_lbl);

    _add_status_lbl = lv_label_create(_panel);
    lv_label_set_text(_add_status_lbl, "");
    lv_obj_set_style_text_font(_add_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(_add_status_lbl, PANEL_W - 20);
    lv_obj_set_pos(_add_status_lbl, 0, 130);

    lv_obj_add_event_cb(_add_ta, [](lv_event_t *e) {
        lv_keyboard_set_textarea(_keyboard, _add_ta);
        lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_FOCUSED, nullptr);
}

void location_picker_init(lv_obj_t *screen) {
    _picker_btn = lv_obj_create(screen);
    lv_obj_set_size(_picker_btn, BTN_W, BTN_H);
    // Lives inline in the status bar itself now -- in the empty gap between
    // the aircraft count and the nav tabs. _picker_btn is created on `screen`
    // (same as before) after status_bar_create() runs in main.cpp, so it
    // still draws on top of the bar without needing to reparent into it.
    // Previously floated top-left over the canvas, then briefly lived atop
    // the per-view filter-button column -- both put it in the way of view
    // content; the status bar is the one piece of chrome every view shares,
    // so this is where it stays regardless of which tab is active.
    lv_obj_set_pos(_picker_btn, LOCATION_CHIP_X, (STATUS_BAR_HEIGHT - BTN_H) / 2);
    lv_obj_set_style_bg_color(_picker_btn, COLOR_ROW, 0);
    lv_obj_set_style_bg_opa(_picker_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_picker_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(_picker_btn, 1, 0);
    lv_obj_set_style_border_opa(_picker_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(_picker_btn, 4, 0);
    lv_obj_set_style_pad_all(_picker_btn, 0, 0);
    lv_obj_clear_flag(_picker_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_picker_btn, picker_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    _picker_lbl = lv_label_create(_picker_btn);
    lv_obj_set_style_text_font(_picker_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_picker_lbl, COLOR_ACCENT, 0);
    lv_obj_center(_picker_lbl);
    update_picker_label();

    // Poll for a completed "add" fetch — the actual work happens on
    // location_poll_task's stack via locations_add_poll(); this just checks
    // for the result (see locations.h / project_p4_heap_constraints memory).
    lv_timer_create([](lv_timer_t *t) {
        if (!_add_in_progress) return;
        bool ok;
        char err[48];
        if (!locations_add_result(&ok, err, sizeof(err))) return;
        _add_in_progress = false;

        if (!_add_status_lbl) return; // user backed out of the add view already
        if (ok) {
            build_list_view(); // back to the list, now showing the new airport
        } else {
            lv_label_set_text(_add_status_lbl, err);
            lv_obj_set_style_text_color(_add_status_lbl, COLOR_ERR, 0);
        }
    }, 300, nullptr);
}
