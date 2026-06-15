#include <Arduino.h>
#include "status_bar.h"
#include "views.h"
#include "../config.h"
#include "../pins_config.h"
#include "../data/fetcher.h"

static lv_obj_t *wifi_icon;
static lv_obj_t *ac_count_label;
static lv_obj_t *update_label;
static lv_obj_t *nav_btns[4];
static lv_obj_t *nav_labels[4];
static lv_obj_t *gear_icon;
static lv_obj_t *auto_label;

static const char *NAV_NAMES[] = {"MAP", "RADAR", "ARR", "STATS"};

#define STATUS_BAR_HEIGHT 30
#define STATUS_BG_COLOR lv_color_hex(0x0d0d1a)
#define STATUS_TEXT_COLOR lv_color_hex(0x888899)
#define STATUS_ACCENT_COLOR lv_color_hex(0x00cc66)

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
    lv_label_set_text(ac_count_label, "0 aircraft");
    lv_obj_set_style_text_color(ac_count_label, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(ac_count_label, &lv_font_montserrat_14, 0);
    lv_obj_align(ac_count_label, LV_ALIGN_LEFT_MID, 42, 0);

    // Nav buttons (center)
    int nav_total_w = 4 * 60 + 3 * 6; // 4 buttons, 6px gaps
    int nav_x0 = (LCD_H_RES - nav_total_w) / 2;
    for (int i = 0; i < 4; i++) {
        nav_btns[i] = lv_obj_create(bar);
        lv_obj_set_size(nav_btns[i], 60, 24);
        lv_obj_set_pos(nav_btns[i], nav_x0 + i * 66, 3);
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
    lv_obj_set_pos(auto_label, nav_x0 + nav_total_w + 8, 8);
    lv_obj_clear_flag(auto_label, LV_OBJ_FLAG_CLICKABLE);

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
    lv_label_set_text_fmt(ac_count_label, "%d aircraft", aircraft_count);

    // Last update
    if (last_update_ms == 0) {
        lv_label_set_text(update_label, "No data");
    } else {
        uint32_t ago = (millis() - last_update_ms) / 1000;
        lv_label_set_text_fmt(update_label, "%lus ago", ago);
    }
}

void status_bar_set_gear_callback(lv_event_cb_t cb) {
    lv_obj_add_event_cb(gear_icon, cb, LV_EVENT_CLICKED, nullptr);
}

void status_bar_set_active_dot(int view_index) {
    for (int i = 0; i < 4; i++) {
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
}

void status_bar_set_auto_indicator(bool visible) {
    if (visible) {
        lv_obj_clear_flag(auto_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(auto_label, LV_OBJ_FLAG_HIDDEN);
    }
}
