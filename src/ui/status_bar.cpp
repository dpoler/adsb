#include <Arduino.h>
#include "status_bar.h"
#include "views.h"
#include "range.h"
#include "display_prefs.h"
#include "trail_menu.h"
#include "../pins_config.h"
#include "../data/fetcher.h"

static lv_obj_t *wifi_icon;
static lv_obj_t *ac_count_label;
static lv_obj_t *update_label;
static lv_obj_t *nav_btns[NUM_VIEWS];
static lv_obj_t *nav_labels[NUM_VIEWS];
static lv_obj_t *gear_icon;
static lv_obj_t *auto_label;
static lv_obj_t *range_chip;
static lv_obj_t *range_lbl;
static lv_obj_t *trails_chip;
static lv_obj_t *tag_chip;
static lv_obj_t *tag_lbl;

static const char *NAV_NAMES[] = {"MAP", "RADAR", "LIST", "STAT"};

#define STATUS_BG_COLOR lv_color_hex(0x0d0d1a)
#define STATUS_TEXT_COLOR lv_color_hex(0x888899)
#define STATUS_ACCENT_COLOR lv_color_hex(0x00cc66)

// Shared size/spacing for every button in the bar -- nav tabs and the
// location/range/TRAIL/TAG chips all use the same width/height so the row
// reads as one consistent family of controls. The picker chip
// (location_picker.cpp, a separate compilation unit) mirrors CHIP_W/CHIP_H.
#define CHIP_W 60
#define CHIP_H 24
#define CHIP_GAP 8

lv_obj_t *status_bar_create(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LCD_H_RES, STATUS_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, STATUS_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Network indicator (left side) — updated dynamically
    wifi_icon = lv_label_create(bar);
    lv_label_set_text(wifi_icon, "...");
    lv_obj_set_style_text_color(wifi_icon, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 8, 0);

    // Aircraft count
    ac_count_label = lv_label_create(bar);
    lv_label_set_text(ac_count_label, "0 AC");
    lv_obj_set_style_text_color(ac_count_label, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(ac_count_label, &lv_font_montserrat_14, 0);
    lv_obj_align(ac_count_label, LV_ALIGN_LEFT_MID, 42, 0);

    // Range chip -- shared by Map/Radar/Arrivals (all three read the same
    // range_get_nm()/range_cycle() global); Stats has no radius concept, so
    // this is hidden there via status_bar_set_active_dot(). Each view already
    // polls range_get_nm() on its own periodic redraw timer to notice a
    // change and re-render, so this chip only has to cycle the shared value
    // and update its own label -- no per-view signaling needed.
    range_chip = lv_obj_create(bar);
    lv_obj_set_size(range_chip, CHIP_W, CHIP_H);
    lv_obj_set_pos(range_chip, LOCATION_CHIP_X + CHIP_W + CHIP_GAP, (STATUS_BAR_HEIGHT - CHIP_H) / 2);
    lv_obj_set_style_bg_color(range_chip, lv_color_hex(0x14142a), 0);
    lv_obj_set_style_bg_opa(range_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(range_chip, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_border_width(range_chip, 1, 0);
    lv_obj_set_style_border_opa(range_chip, LV_OPA_40, 0);
    lv_obj_set_style_radius(range_chip, 4, 0);
    lv_obj_set_style_pad_all(range_chip, 0, 0);
    lv_obj_clear_flag(range_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(range_chip, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(range_lbl, range_label());
    }, LV_EVENT_CLICKED, nullptr);

    range_lbl = lv_label_create(range_chip);
    lv_label_set_text(range_lbl, range_label());
    lv_obj_set_style_text_font(range_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(range_lbl, lv_color_hex(0x4488ff), 0);
    lv_obj_center(range_lbl);

    // Nav buttons (center)
    int nav_total_w = NUM_VIEWS * CHIP_W + (NUM_VIEWS - 1) * 6;
    int nav_x0 = (LCD_H_RES - nav_total_w) / 2;
    for (int i = 0; i < NUM_VIEWS; i++) {
        nav_btns[i] = lv_obj_create(bar);
        lv_obj_set_size(nav_btns[i], CHIP_W, CHIP_H);
        lv_obj_set_pos(nav_btns[i], nav_x0 + i * (CHIP_W + 6), (STATUS_BAR_HEIGHT - CHIP_H) / 2);
        lv_obj_set_style_bg_color(nav_btns[i], STATUS_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(nav_btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(nav_btns[i], STATUS_TEXT_COLOR, 0);
        lv_obj_set_style_border_width(nav_btns[i], 1, 0);
        lv_obj_set_style_border_opa(nav_btns[i], LV_OPA_40, 0);
        lv_obj_set_style_radius(nav_btns[i], 4, 0);
        lv_obj_set_style_pad_all(nav_btns[i], 0, 0);
        lv_obj_clear_flag(nav_btns[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(nav_btns[i], LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(nav_btns[i], [](lv_event_t *e) {
            views_switch_to((int)(intptr_t)lv_event_get_user_data(e));
        }, LV_EVENT_PRESSED, (void *)(intptr_t)i);

        nav_labels[i] = lv_label_create(nav_btns[i]);
        lv_label_set_text(nav_labels[i], NAV_NAMES[i]);
        lv_obj_set_style_text_font(nav_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nav_labels[i], STATUS_TEXT_COLOR, 0);
        lv_obj_center(nav_labels[i]);
    }
    // First button active by default
    lv_obj_set_style_bg_color(nav_btns[0], STATUS_ACCENT_COLOR, 0);
    lv_obj_set_style_text_color(nav_labels[0], lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_opa(nav_btns[0], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nav_btns[0], STATUS_ACCENT_COLOR, 0);

    // AUTO cycle indicator (right of nav buttons)
    auto_label = lv_label_create(bar);
    lv_label_set_text(auto_label, "AUTO");
    lv_obj_set_style_text_font(auto_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(auto_label, STATUS_ACCENT_COLOR, 0);
    lv_obj_set_pos(auto_label, nav_x0 + nav_total_w + 8, (STATUS_BAR_HEIGHT - 16) / 2);
    lv_obj_clear_flag(auto_label, LV_OBJ_FLAG_CLICKABLE);

    // Trails quick-settings chip -- Map/Radar only (Arrivals/Stats have no
    // trails). Opens the trail_menu.cpp popover (on/off, length, clear-now
    // together) instead of directly clearing -- see the "consolidate trail
    // controls" backlog note. status_bar_set_active_dot() shows/hides this
    // chip per view. Starts one CHIP_W past the nav group as a buffer
    // clearing the AUTO indicator.
    trails_chip = lv_obj_create(bar);
    lv_obj_set_size(trails_chip, CHIP_W, CHIP_H);
    lv_obj_set_pos(trails_chip, nav_x0 + nav_total_w + CHIP_W, (STATUS_BAR_HEIGHT - CHIP_H) / 2);
    lv_obj_set_style_bg_color(trails_chip, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(trails_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(trails_chip, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_border_width(trails_chip, 1, 0);
    lv_obj_set_style_border_opa(trails_chip, LV_OPA_40, 0);
    lv_obj_set_style_radius(trails_chip, 4, 0);
    lv_obj_set_style_pad_all(trails_chip, 0, 0);
    lv_obj_clear_flag(trails_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(trails_chip, [](lv_event_t *e) {
        trail_menu_toggle();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *trails_lbl = lv_label_create(trails_chip);
    lv_label_set_text(trails_lbl, "TRAIL");
    lv_obj_set_style_text_font(trails_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(trails_lbl, STATUS_TEXT_COLOR, 0);
    lv_obj_center(trails_lbl);

    // Hide-callsigns chip -- Map/Radar only, same visibility rule as TRAIL. A
    // persistent toggle rather than a momentary action, so it gets a visual
    // "active" state (brighter border/text) instead of always looking the
    // same.
    tag_chip = lv_obj_create(bar);
    lv_obj_set_size(tag_chip, CHIP_W, CHIP_H);
    lv_obj_set_pos(tag_chip, nav_x0 + nav_total_w + CHIP_W + CHIP_W + CHIP_GAP, (STATUS_BAR_HEIGHT - CHIP_H) / 2);
    lv_obj_set_style_bg_color(tag_chip, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(tag_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tag_chip, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_border_width(tag_chip, 1, 0);
    lv_obj_set_style_border_opa(tag_chip, LV_OPA_40, 0);
    lv_obj_set_style_radius(tag_chip, 4, 0);
    lv_obj_set_style_pad_all(tag_chip, 0, 0);
    lv_obj_clear_flag(tag_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tag_chip, [](lv_event_t *e) {
        callsigns_hidden_toggle();
        bool hidden = callsigns_hidden();
        lv_obj_set_style_border_color(tag_chip, hidden ? lv_color_hex(0xccccdd) : STATUS_TEXT_COLOR, 0);
        lv_obj_set_style_border_opa(tag_chip, hidden ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_text_color(tag_lbl, hidden ? lv_color_hex(0xccccdd) : STATUS_TEXT_COLOR, 0);
    }, LV_EVENT_CLICKED, nullptr);

    tag_lbl = lv_label_create(tag_chip);
    lv_label_set_text(tag_lbl, "TAG");
    lv_obj_set_style_text_font(tag_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tag_lbl, STATUS_TEXT_COLOR, 0);
    lv_obj_center(tag_lbl);

    // Gear icon (right side, before update label)
    gear_icon = lv_label_create(bar);
    lv_label_set_text(gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_icon, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(gear_icon, &lv_font_montserrat_16, 0);
    lv_obj_align(gear_icon, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(gear_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear_icon, 10); // easier to tap

    // Last update (right side, shifted left for gear icon)
    update_label = lv_label_create(bar);
    lv_label_set_text(update_label, "--");
    lv_obj_set_style_text_color(update_label, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(update_label, &lv_font_montserrat_14, 0);
    lv_obj_align(update_label, LV_ALIGN_RIGHT_MID, -36, 0);

    return bar;
}

void status_bar_update(bool wifi_connected, int aircraft_count, uint32_t last_update_ms) {
    // Network icon — show type and color by status
    NetType net = fetcher_connection_type();
    if (net == NET_ETHERNET) {
        lv_label_set_text(wifi_icon, "ETH");
        lv_obj_set_style_text_color(wifi_icon, STATUS_ACCENT_COLOR, 0);
    } else if (net == NET_WIFI) {
        lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon, STATUS_ACCENT_COLOR, 0);
    } else {
        lv_label_set_text(wifi_icon, "---");
        lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xcc3333), 0);
    }

    // Aircraft count
    lv_label_set_text_fmt(ac_count_label, "%d AC", aircraft_count);

    // Last update
    if (last_update_ms == 0) {
        lv_label_set_text(update_label, "No data");
    } else {
        uint32_t ago = (millis() - last_update_ms) / 1000;
        lv_label_set_text_fmt(update_label, "%lus", ago);
    }
}

void status_bar_set_gear_callback(lv_event_cb_t cb) {
    lv_obj_add_event_cb(gear_icon, cb, LV_EVENT_CLICKED, nullptr);
}

void status_bar_set_active_dot(int view_index) {
    for (int i = 0; i < NUM_VIEWS; i++) {
        bool active = (i == view_index);
        lv_obj_set_style_bg_color(nav_btns[i],
            active ? STATUS_ACCENT_COLOR : STATUS_BG_COLOR, 0);
        lv_obj_set_style_text_color(nav_labels[i],
            active ? lv_color_hex(0x000000) : STATUS_TEXT_COLOR, 0);
        lv_obj_set_style_border_color(nav_btns[i],
            active ? STATUS_ACCENT_COLOR : STATUS_TEXT_COLOR, 0);
        lv_obj_set_style_border_opa(nav_btns[i],
            active ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    // Range doesn't apply to Stats
    if (view_index == VIEW_STATS) lv_obj_add_flag(range_chip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(range_chip, LV_OBJ_FLAG_HIDDEN);

    // Trails and callsign labels only exist on Map/Radar
    bool map_or_radar = (view_index == VIEW_MAP || view_index == VIEW_RADAR);
    if (map_or_radar) lv_obj_clear_flag(trails_chip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(trails_chip, LV_OBJ_FLAG_HIDDEN);
    if (map_or_radar) lv_obj_clear_flag(tag_chip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(tag_chip, LV_OBJ_FLAG_HIDDEN);
}

void status_bar_set_auto_indicator(bool visible) {
    if (visible) {
        lv_obj_clear_flag(auto_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(auto_label, LV_OBJ_FLAG_HIDDEN);
    }
}
