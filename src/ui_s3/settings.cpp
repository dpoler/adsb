#include <Arduino.h>
#include "settings.h"
#include "../pins_s3.h"
#include "../data/storage.h"
#include <cstdio>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_keyboard = nullptr;
static bool _visible = false;

// Text areas
static lv_obj_t *_ta_ssid = nullptr;
static lv_obj_t *_ta_pass = nullptr;
static lv_obj_t *_ta_lat = nullptr;
static lv_obj_t *_ta_lon = nullptr;

// Controls
static lv_obj_t *_slider_radius = nullptr;
static lv_obj_t *_radius_label = nullptr;
static lv_obj_t *_sw_metric = nullptr;
static lv_obj_t *_sw_alert_mil = nullptr;
static lv_obj_t *_sw_alert_emg = nullptr;
static lv_obj_t *_sw_trails = nullptr;
static lv_obj_t *_slider_trail_len = nullptr;
static lv_obj_t *_trail_len_label = nullptr;
static lv_obj_t *_sw_cycle = nullptr;
static lv_obj_t *_slider_cycle_int = nullptr;
static lv_obj_t *_cycle_int_label = nullptr;

static UserConfig _cfg;
static settings_changed_cb_t _on_change = nullptr;

#define PANEL_W 360
#define PANEL_H 280
#define FIELD_W 180
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)

static lv_obj_t *_focused_ta = nullptr;

static void show_keyboard_for(lv_obj_t *ta) {
    _focused_ta = ta;
    lv_keyboard_set_textarea(_keyboard, ta);
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void ta_focus_cb(lv_event_t *e) {
    show_keyboard_for(lv_event_get_target_obj(e));
}

static void keyboard_ready_cb(lv_event_t *e) {
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    _focused_ta = nullptr;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static lv_obj_t *create_textarea(lv_obj_t *parent, const char *placeholder,
                                  const char *value, int x, int y, bool password = false) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, FIELD_W, 32);
    lv_obj_set_pos(ta, x, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, value);
    if (password) lv_textarea_set_password_mode(ta, true);

    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, ACCENT_COLOR, LV_STATE_FOCUSED);

    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

static lv_obj_t *create_switch(lv_obj_t *parent, int x, int y, bool checked) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(sw, ACCENT_COLOR, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static void save_and_close(lv_event_t *e) {
    strncpy(_cfg.wifi_ssid, lv_textarea_get_text(_ta_ssid), sizeof(_cfg.wifi_ssid) - 1);
    _cfg.wifi_ssid[sizeof(_cfg.wifi_ssid) - 1] = '\0';
    strncpy(_cfg.wifi_pass, lv_textarea_get_text(_ta_pass), sizeof(_cfg.wifi_pass) - 1);
    _cfg.wifi_pass[sizeof(_cfg.wifi_pass) - 1] = '\0';
    _cfg.home_lat = atof(lv_textarea_get_text(_ta_lat));
    _cfg.home_lon = atof(lv_textarea_get_text(_ta_lon));
    _cfg.radius_nm = lv_slider_get_value(_slider_radius);
    _cfg.use_metric = lv_obj_has_state(_sw_metric, LV_STATE_CHECKED);
    _cfg.alert_military = lv_obj_has_state(_sw_alert_mil, LV_STATE_CHECKED);
    _cfg.alert_emergency = lv_obj_has_state(_sw_alert_emg, LV_STATE_CHECKED);
    _cfg.trails_enabled = lv_obj_has_state(_sw_trails, LV_STATE_CHECKED);
    _cfg.trail_max_points = lv_slider_get_value(_slider_trail_len);
    _cfg.cycle_enabled = lv_obj_has_state(_sw_cycle, LV_STATE_CHECKED);
    _cfg.cycle_interval_s = lv_slider_get_value(_slider_cycle_int);

    storage_save_config(_cfg);
    Serial.println("Config saved to NVS");

    if (_on_change) _on_change(&_cfg);

    settings_hide();
}

void settings_init(lv_obj_t *parent) {
    // Semi-transparent overlay
    _overlay = lv_obj_create(parent);
    lv_obj_set_size(_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _overlay) settings_hide();
    }, LV_EVENT_CLICKED, nullptr);

    // Settings panel — scrollable single column
    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_panel, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_panel, 8, 0);
    lv_obj_set_style_border_color(_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_pad_all(_panel, 12, 0);
    lv_obj_add_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_panel, LV_SCROLLBAR_MODE_AUTO);

    // Title
    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 0, 0);

    _cfg = storage_load_config();

    int y = 24;

    // WiFi SSID
    create_label(_panel, "WiFi SSID", 0, y);
    _ta_ssid = create_textarea(_panel, "SSID", _cfg.wifi_ssid, 0, y + 16);
    y += 52;

    // WiFi Password
    create_label(_panel, "WiFi Password", 0, y);
    _ta_pass = create_textarea(_panel, "Password", _cfg.wifi_pass, 0, y + 16, true);
    y += 52;

    // Home coordinates side by side
    create_label(_panel, "Lat", 0, y);
    char lat_str[16], lon_str[16];
    snprintf(lat_str, sizeof(lat_str), "%.4f", _cfg.home_lat);
    snprintf(lon_str, sizeof(lon_str), "%.4f", _cfg.home_lon);
    _ta_lat = create_textarea(_panel, "40.7128", lat_str, 0, y + 16);
    lv_obj_set_width(_ta_lat, 85);

    create_label(_panel, "Lon", 95, y);
    _ta_lon = create_textarea(_panel, "-74.0060", lon_str, 95, y + 16);
    lv_obj_set_width(_ta_lon, 85);

    // Radius
    create_label(_panel, "Radius", 190, y);
    _slider_radius = lv_slider_create(_panel);
    lv_obj_set_size(_slider_radius, 100, 8);
    lv_obj_set_pos(_slider_radius, 190, y + 20);
    lv_slider_set_range(_slider_radius, 5, 150);
    lv_slider_set_value(_slider_radius, _cfg.radius_nm, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_slider_radius, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(_slider_radius, ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_slider_radius, ACCENT_COLOR, LV_PART_KNOB);

    _radius_label = lv_label_create(_panel);
    lv_label_set_text_fmt(_radius_label, "%dnm", _cfg.radius_nm);
    lv_obj_set_style_text_color(_radius_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(_radius_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_radius_label, 296, y + 16);

    lv_obj_add_event_cb(_slider_radius, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(_radius_label, "%dnm", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    y += 52;

    // Toggles — compact row layout
    int sw_x = 120;

    create_label(_panel, "Metric", 0, y);
    _sw_metric = create_switch(_panel, sw_x, y - 2, _cfg.use_metric);

    create_label(_panel, "Mil Alerts", 170, y);
    _sw_alert_mil = create_switch(_panel, 270, y - 2, _cfg.alert_military);
    y += 30;

    create_label(_panel, "Emg Alerts", 0, y);
    _sw_alert_emg = create_switch(_panel, sw_x, y - 2, _cfg.alert_emergency);
    y += 30;

    create_label(_panel, "Trails", 0, y);
    _sw_trails = create_switch(_panel, sw_x, y - 2, _cfg.trails_enabled);

    // Trail length
    _slider_trail_len = lv_slider_create(_panel);
    lv_obj_set_size(_slider_trail_len, 80, 8);
    lv_obj_set_pos(_slider_trail_len, 190, y + 2);
    lv_slider_set_range(_slider_trail_len, 10, 60);
    lv_slider_set_value(_slider_trail_len, _cfg.trail_max_points, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_slider_trail_len, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(_slider_trail_len, ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_slider_trail_len, ACCENT_COLOR, LV_PART_KNOB);

    _trail_len_label = lv_label_create(_panel);
    lv_label_set_text_fmt(_trail_len_label, "%dpts", _cfg.trail_max_points);
    lv_obj_set_style_text_color(_trail_len_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(_trail_len_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_trail_len_label, 276, y - 2);

    lv_obj_add_event_cb(_slider_trail_len, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(_trail_len_label, "%dpts", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    y += 30;

    // Auto-cycle
    create_label(_panel, "Auto-Cycle", 0, y);
    _sw_cycle = create_switch(_panel, sw_x, y - 2, _cfg.cycle_enabled);

    _slider_cycle_int = lv_slider_create(_panel);
    lv_obj_set_size(_slider_cycle_int, 80, 8);
    lv_obj_set_pos(_slider_cycle_int, 190, y + 2);
    lv_slider_set_range(_slider_cycle_int, 15, 120);
    lv_slider_set_value(_slider_cycle_int, _cfg.cycle_interval_s, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_slider_cycle_int, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(_slider_cycle_int, ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_slider_cycle_int, ACCENT_COLOR, LV_PART_KNOB);

    _cycle_int_label = lv_label_create(_panel);
    lv_label_set_text_fmt(_cycle_int_label, "%ds", _cfg.cycle_interval_s);
    lv_obj_set_style_text_color(_cycle_int_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(_cycle_int_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_cycle_int_label, 276, y - 2);

    lv_obj_add_event_cb(_slider_cycle_int, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(_cycle_int_label, "%ds", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    y += 36;

    // Save button
    lv_obj_t *save_btn = lv_button_create(_panel);
    lv_obj_set_size(save_btn, 100, 32);
    lv_obj_set_pos(save_btn, (PANEL_W - 100 - 24) / 2, y);
    lv_obj_set_style_bg_color(save_btn, ACCENT_COLOR, 0);
    lv_obj_set_style_radius(save_btn, 6, 0);

    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_14, 0);
    lv_obj_center(save_label);

    lv_obj_add_event_cb(save_btn, save_and_close, LV_EVENT_CLICKED, nullptr);

    // On-screen keyboard
    _keyboard = lv_keyboard_create(_overlay);
    lv_obj_set_size(_keyboard, LCD_H_RES, 160);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_CANCEL, nullptr);
}

void settings_show() {
    if (_visible) return;
    _visible = true;

    _cfg = storage_load_config();
    lv_textarea_set_text(_ta_ssid, _cfg.wifi_ssid);
    lv_textarea_set_text(_ta_pass, _cfg.wifi_pass);

    char lat_str[16], lon_str[16];
    snprintf(lat_str, sizeof(lat_str), "%.4f", _cfg.home_lat);
    snprintf(lon_str, sizeof(lon_str), "%.4f", _cfg.home_lon);
    lv_textarea_set_text(_ta_lat, lat_str);
    lv_textarea_set_text(_ta_lon, lon_str);

    lv_slider_set_value(_slider_radius, _cfg.radius_nm, LV_ANIM_OFF);
    lv_label_set_text_fmt(_radius_label, "%dnm", _cfg.radius_nm);

    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_metric, LV_STATE_CHECKED);

    if (_cfg.alert_military) lv_obj_add_state(_sw_alert_mil, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_alert_mil, LV_STATE_CHECKED);

    if (_cfg.alert_emergency) lv_obj_add_state(_sw_alert_emg, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_alert_emg, LV_STATE_CHECKED);

    if (_cfg.trails_enabled) lv_obj_add_state(_sw_trails, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_trails, LV_STATE_CHECKED);

    lv_slider_set_value(_slider_trail_len, _cfg.trail_max_points, LV_ANIM_OFF);
    lv_label_set_text_fmt(_trail_len_label, "%dpts", _cfg.trail_max_points);

    if (_cfg.cycle_enabled) lv_obj_add_state(_sw_cycle, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_cycle, LV_STATE_CHECKED);

    lv_slider_set_value(_slider_cycle_int, _cfg.cycle_interval_s, LV_ANIM_OFF);
    lv_label_set_text_fmt(_cycle_int_label, "%ds", _cfg.cycle_interval_s);

    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_hide() {
    if (!_visible) return;
    _visible = false;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool settings_is_visible() {
    return _visible;
}

void settings_set_change_callback(settings_changed_cb_t cb) {
    _on_change = cb;
}
