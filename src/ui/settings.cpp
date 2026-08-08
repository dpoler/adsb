#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include "settings.h"
#include "screensaver.h"
#include "stats.h" // stats_get()->boot_time -- STATUS tab's UPTIME reading
#include "../pins_config.h"
#include "../data/storage.h"
#include "../data/ota.h"
#include "../data/fetcher.h"
#include "../data/error_log.h"
#include "../version.h"
#include <cstdio>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_keyboard = nullptr;
static bool _visible = false;
static uint32_t _shown_at_ms = 0;

// Tabs -- CONFIG (editable, has a Save button) vs STATUS (read-only device/
// network/system/error readouts, formerly the Stats screen's DEVICE column
// -- see stats_view.cpp). Two sibling containers under _panel, toggled via
// LV_OBJ_FLAG_HIDDEN; only one is ever visible at a time.
static lv_obj_t *_tab_btn_config = nullptr;
static lv_obj_t *_tab_btn_status = nullptr;
static lv_obj_t *_tab_config = nullptr;
static lv_obj_t *_tab_status = nullptr;
static lv_obj_t *_save_btn = nullptr;
static int _active_tab = 0; // 0=CONFIG, 1=STATUS

// Text areas
static lv_obj_t *_ta_ssid = nullptr;
static lv_obj_t *_ta_pass = nullptr;

// Controls
static lv_obj_t *_ta_radius[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *_sw_metric = nullptr;
static lv_obj_t *_sw_ethernet = nullptr;
static lv_obj_t *_btn_show_pass = nullptr;
static lv_obj_t *_ota_status_lbl = nullptr;
static lv_obj_t *_ota_btn = nullptr;
static lv_obj_t *_ota_btn_lbl = nullptr;
static lv_timer_t *_ota_timer = nullptr;

// STATUS tab -- network
static lv_obj_t *_ip_val = nullptr;
static lv_obj_t *_rssi_val = nullptr;
static lv_obj_t *_fetch_val = nullptr;
static lv_obj_t *_bytes_val = nullptr;
static lv_obj_t *_latency_val = nullptr;
static lv_obj_t *_airportdb_val = nullptr; // never entered on-device -- this is the only on-device confirmation a token is set

// STATUS tab -- system
static lv_obj_t *_heap_val = nullptr;
static lv_obj_t *_uptime_val = nullptr;
static lv_obj_t *_psram_val = nullptr;
static lv_obj_t *_temp_val = nullptr;
static lv_obj_t *_fps_val = nullptr;
static lv_obj_t *_tasks_val = nullptr;
static lv_obj_t *_lvgl_objs_val = nullptr;
static lv_obj_t *_flash_val = nullptr;

// STATUS tab -- error log
static lv_obj_t *_err_count_lbl = nullptr;
static lv_obj_t *_err_list_lbl = nullptr;

// STATUS tab -- FPS measurement (counts this timer's own tick rate, see the
// timer below -- ported as-is from stats_view.cpp)
static uint32_t _frame_count = 0;
static uint32_t _fps_last_time = 0;
static uint16_t _fps = 0;

static UserConfig _cfg;

// Callback for config changes (set by main)
static settings_changed_cb_t _on_change = nullptr;

// Single 330px-wide content column per tab -- WiFi/ethernet/range/metric
// (CONFIG) and firmware/network/system/errors (STATUS, moved here from
// stats_view.cpp's old DEVICE column) is all that's left across the two of
// them (Home lat/lon, trails, GND, Military/Emergency alerts, and
// Auto-Cycle all moved out over time to the location picker/VIEW menu/
// filter column; the airportdb.io token was dropped from this panel
// entirely -- see below), so the old wide two-column 820px layout was
// mostly empty space by the end. Sized to fit exactly what remains, not
// to match the VIEW-menu-style small anchored popovers (status_bar.cpp)
// -- this stays a centered modal since WiFi credential entry benefits
// from more room for the on-screen keyboard than a 270px popover gives.
#define PANEL_W 370
#define PANEL_H 570 // +10 vs. the pre-tabs 560 -- Firmware Update moved back onto CONFIG (see settings_init()) needed a bit more room than the tab containers' original budget left
#define FIELD_W 280
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)
#define OTA_OK_COLOR lv_color_hex(0x44cc88)
#define OTA_WARN_COLOR lv_color_hex(0xccaa00)
#define OTA_ERR_COLOR lv_color_hex(0xcc4444)
// STATUS tab's NETWORK/SYSTEM readouts (formerly stats_view.cpp's DEVICE
// column) reuse the same two colors above rather than a second pair of
// near-identical hex values -- OTA_OK_COLOR/OTA_WARN_COLOR were already
// exactly stats_view.cpp's old SYS_COLOR/WARN_COLOR.
#define SYS_COLOR OTA_OK_COLOR
#define WARN_COLOR OTA_WARN_COLOR

static lv_obj_t *_focused_ta = nullptr;

static void show_keyboard_for(lv_obj_t *ta) {
    _focused_ta = ta;
    lv_keyboard_set_textarea(_keyboard, ta);
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void ta_focus_cb(lv_event_t *e) {
    show_keyboard_for(lv_event_get_target_obj(e));
    // Range Presets pass LV_KEYBOARD_MODE_NUMBER as user_data (digits, "+/-",
    // "." on one layout instead of buried on the alpha keyboard); every
    // other field here leaves user_data null, which is also
    // LV_KEYBOARD_MODE_TEXT_LOWER (0).
    lv_keyboard_set_mode(_keyboard, (lv_keyboard_mode_t)(intptr_t)lv_event_get_user_data(e));
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
    lv_obj_set_size(ta, FIELD_W, 36);
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

// Everything below (through count_lvgl_objects) is ported from
// stats_view.cpp's old DEVICE column, now the STATUS tab -- see the section
// header comment in settings_init() for the full move rationale.

static void create_section_header(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

// Helper for an inline "HEADER  value" row -- header and value share one line.
static lv_obj_t *create_inline_row(lv_obj_t *parent, const char *header, int x, int y,
                                    lv_color_t val_color, int val_off) {
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, header);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, LABEL_COLOR, 0);
    lv_obj_set_pos(h, x, y);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, val_color, 0);
    lv_obj_set_pos(v, x + val_off, y);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

// Helper for a label pair (header above, value below) -- used for the
// compact multi-item SYSTEM row.
static lv_obj_t *create_stat_pair(lv_obj_t *parent, const char *header, int x, int y,
                                   lv_color_t val_color) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, header);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, val_color, 0);
    lv_obj_set_pos(val, x, y + 16);
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);
    return val;
}

static int count_lvgl_objects(lv_obj_t *obj) {
    int n = 1;
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        n += count_lvgl_objects(lv_obj_get_child(obj, i));
    }
    return n;
}

// Refreshes every STATUS tab reading -- ported from stats_view.cpp's
// refresh_stats(), minus the parts that stayed on the INFO screen (current
// traffic / location stats). Runs on its own timer regardless of whether
// STATUS is the visible tab or Settings is even open -- same reasoning as
// the OTA refresh above and stats_view.cpp's original refresh_stats(): the
// widgets exist for the whole session (created once at boot), so updating
// hidden labels is cheap and keeps this simple.
static void status_tab_refresh(lv_timer_t *t) {
    (void)t;
    const FetcherStats *fs = fetcher_get_stats();

    // === NETWORK ===
    if (fs->ip_addr[0]) {
        lv_label_set_text(_ip_val, fs->ip_addr);
    }
    lv_label_set_text_fmt(_fetch_val, "%lu ok / %lu err", (unsigned long)fs->fetch_ok, (unsigned long)fs->fetch_fail);

    if (fs->bytes_received > 1048576) {
        lv_label_set_text_fmt(_bytes_val, "%.1fMB", (double)fs->bytes_received / 1048576.0);
    } else {
        lv_label_set_text_fmt(_bytes_val, "%luKB", (unsigned long)(fs->bytes_received / 1024));
    }
    if (fs->last_fetch_ms > 0) {
        lv_label_set_text_fmt(_latency_val, "%lums", (unsigned long)fs->last_fetch_ms);
    }

    NetType net = fetcher_connection_type();
    if (net == NET_ETHERNET) {
        lv_label_set_text(_rssi_val, "ETH 100M");
    } else if (net == NET_WIFI) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            lv_label_set_text_fmt(_rssi_val, "WiFi %d dBm", ap_info.rssi);
        } else {
            lv_label_set_text(_rssi_val, "WiFi --");
        }
    } else {
        lv_label_set_text(_rssi_val, "No link");
    }

    // Token is only ever set via tools/configure_device.sh/.ps1's TOKEN=
    // serial command -- this is the only on-device confirmation a value
    // landed. "Set", not "Configured"/"Valid" -- this only checks the field
    // is non-empty, it does NOT mean the token actually authenticates (that
    // needs a live fetch, see TOKEN_VERIFY in serial_config.cpp, which this
    // row deliberately doesn't trigger automatically -- reported: an
    // invalid token still showed as "Configured" here, which read as "this
    // is fine" when it very much wasn't -- confirmed only by a real add-
    // airport attempt failing).
    if (g_config.airportdb_token[0]) {
        lv_obj_set_style_text_color(_airportdb_val, SYS_COLOR, 0);
        lv_label_set_text(_airportdb_val, g_config.airportdb_enabled ? "Set / on" : "Set / off");
    } else {
        lv_obj_set_style_text_color(_airportdb_val, WARN_COLOR, 0);
        lv_label_set_text(_airportdb_val, "Not set");
    }

    // === SYSTEM ===
    uint32_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    lv_label_set_text_fmt(_heap_val, "%luK / %luK min", (unsigned long)(heap_free / 1024), (unsigned long)(heap_min / 1024));
    lv_label_set_text_fmt(_psram_val, "%.1fM free", (double)psram_free / (1024.0 * 1024.0));

    uint32_t uptime_s = (millis() - stats_get()->boot_time) / 1000;
    int hrs = uptime_s / 3600;
    int mins = (uptime_s % 3600) / 60;
    int secs = uptime_s % 60;
    lv_label_set_text_fmt(_uptime_val, "%02d:%02d:%02d", hrs, mins, secs);

    float temp = temperatureRead();
    if (temp > 0) {
        lv_color_t tc = temp > 70 ? lv_color_hex(0xff3333) : temp > 55 ? WARN_COLOR : SYS_COLOR;
        lv_obj_set_style_text_color(_temp_val, tc, 0);
        lv_label_set_text_fmt(_temp_val, "%.0fC", (double)temp);
    } else {
        lv_label_set_text(_temp_val, "N/A");
    }

    lv_label_set_text_fmt(_fps_val, "%d", _fps);
    lv_label_set_text_fmt(_tasks_val, "%lu", (unsigned long)uxTaskGetNumberOfTasks());

    static uint32_t last_obj_count_time = 0;
    static int cached_obj_count = 0;
    uint32_t now = millis();
    if (now - last_obj_count_time > 5000) {
        cached_obj_count = count_lvgl_objects(lv_screen_active());
        last_obj_count_time = now;
    }
    lv_label_set_text_fmt(_lvgl_objs_val, "%d", cached_obj_count);

    lv_label_set_text_fmt(_flash_val, "%.1f%%", 74.6); // static — compiled into binary

    // === ERRORS ===
    uint32_t err_total = error_log_total_count();
    lv_label_set_text_fmt(_err_count_lbl, "(%lu)", (unsigned long)err_total);

    ErrorSnapshot snap = error_log_snapshot();
    if (snap.count == 0) {
        lv_label_set_text(_err_list_lbl, "(none)");
    } else {
        static char err_buf[512];
        int pos = 0;
        uint32_t now_ms = millis();
        // Show newest first
        for (int i = snap.count - 1; i >= 0 && pos < (int)sizeof(err_buf) - 60; i--) {
            uint32_t age_s = (now_ms - snap.entries[i].timestamp) / 1000;
            int m = age_s / 60;
            int s = age_s % 60;
            pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
                "%dm%02ds %s\n", m, s, snap.entries[i].msg);
        }
        if (pos > 0) err_buf[pos - 1] = '\0'; // strip trailing newline
        lv_label_set_text(_err_list_lbl, err_buf);
    }
}

// Reflects the shared ota_status/ota_latest_tag/ota_progress state (set by
// ota_poll(), driven from location_poll_task -- see data/ota.h) into the
// panel's status line and button. Polled on a timer rather than event-
// driven since ota_poll() runs on a different task with no callback hook
// back into LVGL; this is the same pattern the rest of this app uses for
// any state a background task updates asynchronously.
static void ota_ui_refresh(lv_timer_t *t) {
    (void)t;
    char buf[48];
    switch (ota_status) {
        case OTA_IDLE:
            lv_label_set_text(_ota_status_lbl, "Not checked yet");
            lv_obj_set_style_text_color(_ota_status_lbl, LABEL_COLOR, 0);
            lv_label_set_text(_ota_btn_lbl, "Check for Update");
            lv_obj_clear_state(_ota_btn, LV_STATE_DISABLED);
            break;
        case OTA_CHECKING:
            lv_label_set_text(_ota_status_lbl, "Checking...");
            lv_obj_set_style_text_color(_ota_status_lbl, OTA_WARN_COLOR, 0);
            lv_label_set_text(_ota_btn_lbl, "Checking...");
            lv_obj_add_state(_ota_btn, LV_STATE_DISABLED);
            break;
        case OTA_UP_TO_DATE:
            lv_label_set_text(_ota_status_lbl, "Up to date");
            lv_obj_set_style_text_color(_ota_status_lbl, OTA_OK_COLOR, 0);
            lv_label_set_text(_ota_btn_lbl, "Check for Update");
            lv_obj_clear_state(_ota_btn, LV_STATE_DISABLED);
            break;
        case OTA_AVAILABLE:
            snprintf(buf, sizeof(buf), "Update available: %s", ota_latest_tag);
            lv_label_set_text(_ota_status_lbl, buf);
            lv_obj_set_style_text_color(_ota_status_lbl, OTA_WARN_COLOR, 0);
            snprintf(buf, sizeof(buf), "Update to %s", ota_latest_tag);
            lv_label_set_text(_ota_btn_lbl, buf);
            lv_obj_clear_state(_ota_btn, LV_STATE_DISABLED);
            break;
        case OTA_DOWNLOADING:
            snprintf(buf, sizeof(buf), "Downloading... %d%%", ota_progress);
            lv_label_set_text(_ota_status_lbl, buf);
            lv_obj_set_style_text_color(_ota_status_lbl, OTA_WARN_COLOR, 0);
            lv_label_set_text(_ota_btn_lbl, "Updating...");
            lv_obj_add_state(_ota_btn, LV_STATE_DISABLED);
            break;
        case OTA_DONE:
            // Device reboots itself right after this (ota.cpp's do_update()
            // calls ESP.restart() on success) -- this state is essentially
            // never visible, but handled in case that timing ever changes.
            lv_label_set_text(_ota_status_lbl, "Update complete, rebooting...");
            lv_obj_set_style_text_color(_ota_status_lbl, OTA_OK_COLOR, 0);
            lv_obj_add_state(_ota_btn, LV_STATE_DISABLED);
            break;
        case OTA_ERROR:
            lv_label_set_text(_ota_status_lbl, "Check failed -- try again");
            lv_obj_set_style_text_color(_ota_status_lbl, OTA_ERR_COLOR, 0);
            lv_label_set_text(_ota_btn_lbl, "Check for Update");
            lv_obj_clear_state(_ota_btn, LV_STATE_DISABLED);
            break;
    }
}

static void ota_btn_clicked(lv_event_t *e) {
    (void)e;
    if (ota_status == OTA_AVAILABLE) ota_request_update();
    else if (ota_status != OTA_CHECKING && ota_status != OTA_DOWNLOADING) ota_request_check();
}

static void save_and_close(lv_event_t *e) {
    bool old_use_ethernet = _cfg.use_ethernet;
    char old_wifi_ssid[sizeof(_cfg.wifi_ssid)];
    char old_wifi_pass[sizeof(_cfg.wifi_pass)];
    strlcpy(old_wifi_ssid, _cfg.wifi_ssid, sizeof(old_wifi_ssid));
    strlcpy(old_wifi_pass, _cfg.wifi_pass, sizeof(old_wifi_pass));

    // Read values from text areas
    strncpy(_cfg.wifi_ssid, lv_textarea_get_text(_ta_ssid), sizeof(_cfg.wifi_ssid) - 1);
    _cfg.wifi_ssid[sizeof(_cfg.wifi_ssid) - 1] = '\0';
    for (char *p = _cfg.wifi_ssid; *p; p++) if (*p == '\r' || *p == '\n') *p = '\0';
    strncpy(_cfg.wifi_pass, lv_textarea_get_text(_ta_pass), sizeof(_cfg.wifi_pass) - 1);
    _cfg.wifi_pass[sizeof(_cfg.wifi_pass) - 1] = '\0';
    for (char *p = _cfg.wifi_pass; *p; p++) if (*p == '\r' || *p == '\n') *p = '\0';
    // airportdb_token is never edited here -- see the layout comment below --
    // so _cfg's copy (freshly reloaded in settings_show()) is left untouched
    // and just gets written back as-is.
    for (int i = 0; i < 4; i++) {
        int v = atoi(lv_textarea_get_text(_ta_radius[i]));
        if (v < 1) v = 1;
        if (v > 500) v = 500;
        _cfg.radius_presets[i] = v;
    }
    // Sort ascending so range module receives them in order
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 4; j++)
            if (_cfg.radius_presets[i] > _cfg.radius_presets[j]) {
                int tmp = _cfg.radius_presets[i];
                _cfg.radius_presets[i] = _cfg.radius_presets[j];
                _cfg.radius_presets[j] = tmp;
            }
    _cfg.radius_nm = _cfg.radius_presets[3]; // max preset = API query radius
    _cfg.use_metric = lv_obj_has_state(_sw_metric, LV_STATE_CHECKED);
    _cfg.use_ethernet = lv_obj_has_state(_sw_ethernet, LV_STATE_CHECKED);
    // alert_military/alert_emergency are no longer set from a widget here --
    // moved to the VIEW menu's ALERTS section (view_menu.cpp), which writes
    // g_config directly. _cfg already picked up whatever that last set, via
    // the fresh storage_load_config() at the top of settings_show() --
    // nothing here touches those two fields, so this save can't clobber
    // them back to a stale value.

    storage_save_config(_cfg);
    Serial.println("Config saved to NVS");

    if (_on_change) _on_change(&_cfg);

    settings_hide();

    // Network mode change, or a new SSID/password, requires a reboot --
    // WiFi credentials are only ever read at boot (fetcher.cpp), same as
    // every other path that sets them (tools/configure_device.sh/.ps1's
    // WIFI_SSID=/WIFI_PASS= always reboots after too). This save previously
    // only checked the ETH/WiFi mode switch, silently saving new
    // credentials to NVS without ever applying them until some *other*
    // reboot happened to come along -- reported after a factory reset: the
    // on-screen keyboard looked like the way to fix a missing WiFi setup,
    // but Save alone did nothing.
    bool wifi_changed = strcmp(_cfg.wifi_ssid, old_wifi_ssid) != 0 ||
                         strcmp(_cfg.wifi_pass, old_wifi_pass) != 0;
    if (_cfg.use_ethernet != old_use_ethernet || wifi_changed) {
        Serial.println("Network settings changed, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
    }
}

// Switches which tab's content container is visible and updates both tab
// buttons' pressed/unpressed styling. Also toggles the Save button --
// STATUS has nothing to save (all read-only), so it only makes sense while
// CONFIG is showing.
static void show_tab(int tab) {
    _active_tab = tab;
    lv_obj_t *lbl_config = lv_obj_get_child(_tab_btn_config, 0);
    lv_obj_t *lbl_status = lv_obj_get_child(_tab_btn_status, 0);
    if (tab == 0) {
        lv_obj_clear_flag(_tab_config, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_tab_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_save_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(_tab_btn_config, ACCENT_COLOR, 0);
        lv_obj_set_style_text_color(lbl_config, lv_color_black(), 0);
        lv_obj_set_style_bg_color(_tab_btn_status, lv_color_hex(0x1a1a3a), 0);
        lv_obj_set_style_text_color(lbl_status, LABEL_COLOR, 0);
    } else {
        lv_obj_add_flag(_tab_config, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_tab_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_save_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(_tab_btn_status, ACCENT_COLOR, 0);
        lv_obj_set_style_text_color(lbl_status, lv_color_black(), 0);
        lv_obj_set_style_bg_color(_tab_btn_config, lv_color_hex(0x1a1a3a), 0);
        lv_obj_set_style_text_color(lbl_config, LABEL_COLOR, 0);
    }
}

static lv_obj_t *create_tab_button(lv_obj_t *parent, const char *label, int x, int w) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, 32);
    lv_obj_set_pos(btn, x, 32);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_center(lbl);
    return btn;
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

    // Tap overlay background to close. The _shown_at_ms grace window guards
    // against the panel closing on the same tap that opened it -- on the
    // CrowPanel board's touch hardware, a single physical tap on the gear
    // icon was sometimes producing a second, near-immediate CLICKED event
    // that landed on the overlay once it appeared over the same screen
    // location, closing the panel before it was ever visible to the user.
    lv_obj_add_event_cb(_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _overlay && millis() - _shown_at_ms > 400) settings_hide();
    }, LV_EVENT_CLICKED, nullptr);

    // Settings panel (centered)
    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_panel, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_panel, 12, 0);
    lv_obj_set_style_border_color(_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_pad_all(_panel, 20, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 0, 0);

    // Load current config
    _cfg = storage_load_config();

    // Tab buttons -- CONFIG (editable, has Save) / STATUS (read-only device
    // readouts, formerly the Stats/INFO screen's DEVICE column -- see
    // stats_view.cpp). 330 = PANEL_W(370) - pad_all*2(20*2), the full
    // content width; two buttons split it with a 10px gap.
    _tab_btn_config = create_tab_button(_panel, "CONFIG", 0, 160);
    _tab_btn_status = create_tab_button(_panel, "STATUS", 170, 160);
    lv_obj_add_event_cb(_tab_btn_config, [](lv_event_t *e) { show_tab(0); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(_tab_btn_status, [](lv_event_t *e) { show_tab(1); }, LV_EVENT_CLICKED, nullptr);

    // Tab content containers -- siblings under _panel, one hidden at a time
    // via show_tab(). STATUS is left scrollable (NETWORK+SYSTEM+ERRORS,
    // including a variable-length error list, doesn't reliably fit a fixed
    // 330x400 box); CONFIG's content is short and fixed enough not to need
    // it.
    _tab_config = lv_obj_create(_panel);
    lv_obj_set_size(_tab_config, 330, 400);
    lv_obj_set_pos(_tab_config, 0, 74);
    lv_obj_set_style_bg_opa(_tab_config, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tab_config, 0, 0);
    lv_obj_set_style_pad_all(_tab_config, 0, 0);
    lv_obj_clear_flag(_tab_config, LV_OBJ_FLAG_SCROLLABLE);

    _tab_status = lv_obj_create(_panel);
    lv_obj_set_size(_tab_status, 330, 400);
    lv_obj_set_pos(_tab_status, 0, 74);
    lv_obj_set_style_bg_opa(_tab_status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tab_status, 0, 0);
    lv_obj_set_style_pad_all(_tab_status, 0, 0);
    lv_obj_set_scroll_dir(_tab_status, LV_DIR_VER);
    lv_obj_add_flag(_tab_status, LV_OBJ_FLAG_HIDDEN); // CONFIG shown by default -- see show_tab(0) below

    // ============================================================
    // CONFIG tab -- everything else that used to live directly on the panel
    // has moved: Home lat/lon and airport-by-ICAO entry to the location
    // picker's add-flow, Trails/Tags/Secondary-locations/Alerts to the
    // status bar's VIEW chip popover, GND to a quick-access filter-column
    // button, and Auto-Cycle removed outright. The airportdb.io token field
    // was dropped entirely (not just moved) -- it's never typed in on the
    // device itself, only via tools/configure_device.sh/.ps1's TOKEN=
    // serial command; the STATUS tab shows whether one is currently set.
    // What's left is WiFi, network mode, range presets, and units (WiFi/
    // Ethernet grouped as "how this thing gets online", then the
    // display-affecting settings). Firmware Update moved to STATUS -- it's
    // not something you "Save," it has its own instant-action button, so it
    // fits STATUS's device-identity grouping better than a Save-gated tab.
    // ============================================================

    // WiFi
    create_label(_tab_config, "WiFi SSID", 0, 0);
    _ta_ssid = create_textarea(_tab_config, "SSID", _cfg.wifi_ssid, 0, 18);

    create_label(_tab_config, "WiFi Password", 0, 60);
    _ta_pass = create_textarea(_tab_config, "Password", _cfg.wifi_pass, 0, 78, true);

    _btn_show_pass = lv_button_create(_tab_config);
    lv_obj_set_size(_btn_show_pass, 34, 36);
    lv_obj_set_pos(_btn_show_pass, FIELD_W + 4, 78);
    lv_obj_set_style_bg_color(_btn_show_pass, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(_btn_show_pass, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_btn_show_pass, 1, 0);
    lv_obj_set_style_radius(_btn_show_pass, 4, 0);
    lv_obj_set_style_shadow_width(_btn_show_pass, 0, 0);
    { lv_obj_t *lbl = lv_label_create(_btn_show_pass);
      lv_label_set_text(lbl, LV_SYMBOL_EYE_OPEN);
      lv_obj_set_style_text_color(lbl, lv_color_hex(0x8888aa), 0);
      lv_obj_center(lbl); }
    lv_obj_add_event_cb(_btn_show_pass, [](lv_event_t *e) {
        bool pw = lv_textarea_get_password_mode(_ta_pass);
        lv_textarea_set_password_mode(_ta_pass, !pw);
        lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target_obj(e), 0);
        lv_label_set_text(lbl, pw ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    }, LV_EVENT_CLICKED, nullptr);

    // Network mode (Ethernet toggle — off=WiFi, on=Ethernet)
    create_label(_tab_config, "Ethernet", 0, 122);
    _sw_ethernet = create_switch(_tab_config, 110, 120, _cfg.use_ethernet);
    lv_obj_t *net_hint = lv_label_create(_tab_config);
    lv_label_set_text(net_hint, "(requires reboot)");
    lv_obj_set_style_text_color(net_hint, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(net_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(net_hint, 164, 124);

    // Range Presets — 4 configurable text fields (nm, 1-500). Deliberately
    // more vertical room above this than between the tightly-grouped WiFi/
    // Ethernet rows above -- those three are "how this thing gets online"
    // and read as one group; this and Metric below are separate settings,
    // not part of that group.
    create_label(_tab_config, "Range Presets (nm, 1-500)", 0, 170);
    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        _ta_radius[i] = lv_textarea_create(_tab_config);
        lv_obj_set_size(_ta_radius[i], 60, 36);
        lv_obj_set_pos(_ta_radius[i], i * 66, 190);
        lv_textarea_set_one_line(_ta_radius[i], true);
        lv_textarea_set_text(_ta_radius[i], rbuf);
        lv_obj_set_style_bg_color(_ta_radius[i], lv_color_hex(0x1a1a3a), 0);
        lv_obj_set_style_text_color(_ta_radius[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(_ta_radius[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_border_color(_ta_radius[i], lv_color_hex(0x333366), 0);
        lv_obj_set_style_border_width(_ta_radius[i], 1, 0);
        lv_obj_set_style_border_color(_ta_radius[i], ACCENT_COLOR, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(_ta_radius[i], ta_focus_cb, LV_EVENT_FOCUSED,
                            (void *)(intptr_t)LV_KEYBOARD_MODE_NUMBER);
    }

    // Metric -- same extra breathing room above as Range Presets got, for
    // the same reason (a standalone setting, not part of the WiFi/Ethernet
    // group above).
    create_label(_tab_config, "Metric Units", 0, 254);
    _sw_metric = create_switch(_tab_config, 110, 252, _cfg.use_metric);

    // Firmware update -- same extra breathing room above as Range Presets/
    // Metric got, for the same reason (a standalone group). Briefly moved to
    // the STATUS tab alongside NETWORK/SYSTEM (device-identity grouping),
    // moved back here per feedback -- it fits CONFIG's vertical budget
    // better than STATUS's, and unlike NETWORK/SYSTEM it's not a live
    // reading that needs the always-visible STATUS tab, just an occasional
    // action.
    create_label(_tab_config, "Firmware Update", 0, 291);
    { lv_obj_t *ver = lv_label_create(_tab_config);
      char vbuf[40];
      snprintf(vbuf, sizeof(vbuf), "Running: %s", FIRMWARE_VERSION_STR);
      lv_label_set_text(ver, vbuf);
      lv_obj_set_style_text_color(ver, lv_color_hex(0xccccdd), 0);
      lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
      lv_obj_set_pos(ver, 0, 311); }

    _ota_status_lbl = lv_label_create(_tab_config);
    lv_obj_set_style_text_font(_ota_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_ota_status_lbl, 0, 333);

    _ota_btn = lv_button_create(_tab_config);
    lv_obj_set_size(_ota_btn, FIELD_W, 36);
    lv_obj_set_pos(_ota_btn, 0, 355);
    lv_obj_set_style_bg_color(_ota_btn, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(_ota_btn, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_ota_btn, 1, 0);
    lv_obj_set_style_radius(_ota_btn, 6, 0);
    lv_obj_set_style_bg_color(_ota_btn, lv_color_hex(0x1a1a3a), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(_ota_btn, lv_color_hex(0x555577), LV_STATE_DISABLED);
    _ota_btn_lbl = lv_label_create(_ota_btn);
    lv_label_set_text(_ota_btn_lbl, "Check for Update");
    lv_obj_set_style_text_color(_ota_btn_lbl, lv_color_hex(0xccccdd), 0);
    lv_obj_set_style_text_font(_ota_btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(_ota_btn_lbl);
    lv_obj_add_event_cb(_ota_btn, ota_btn_clicked, LV_EVENT_CLICKED, nullptr);

    ota_ui_refresh(nullptr);
    // 500ms is fine -- an OTA check/download takes seconds, not something
    // that needs snappier feedback than that, and this only runs while the
    // Settings panel object exists (created once at boot, never destroyed).
    _ota_timer = lv_timer_create(ota_ui_refresh, 500, nullptr);

    // ============================================================
    // STATUS tab -- read-only device/network/system/error readouts, moved
    // here wholesale from stats_view.cpp's old DEVICE column (see that
    // file's git history for the original) so the INFO screen (renamed
    // from STAT) can dedicate its full width to live aircraft/location
    // data.
    // ============================================================

    // NETWORK
    int net_y = 0;
    create_section_header(_tab_status, "NETWORK", 0, net_y);
    _ip_val = create_inline_row(_tab_status, "IP", 0, net_y + 18, SYS_COLOR, 90);
    _rssi_val = create_inline_row(_tab_status, "LINK", 0, net_y + 36, SYS_COLOR, 90);
    _fetch_val = create_inline_row(_tab_status, "FETCHES", 0, net_y + 54, SYS_COLOR, 90);
    _bytes_val = create_inline_row(_tab_status, "RX DATA", 0, net_y + 72, SYS_COLOR, 90);
    _latency_val = create_inline_row(_tab_status, "LATENCY", 0, net_y + 90, SYS_COLOR, 90);
    _airportdb_val = create_inline_row(_tab_status, "AIRPORTDB", 0, net_y + 108, SYS_COLOR, 90);

    // SYSTEM
    int sy = net_y + 126 + 14;
    create_section_header(_tab_status, "SYSTEM", 0, sy);
    _heap_val = create_stat_pair(_tab_status, "HEAP", 0, sy + 18, SYS_COLOR);
    _uptime_val = create_stat_pair(_tab_status, "UPTIME", 150, sy + 18, SYS_COLOR);
    _psram_val = create_stat_pair(_tab_status, "PSRAM", 0, sy + 52, SYS_COLOR);

    // Compact row: TEMP / FPS / TASKS / OBJS / FLASH
    int sr2 = sy + 86;
    _temp_val = create_stat_pair(_tab_status, "TEMP", 0, sr2, SYS_COLOR);
    _fps_val = create_stat_pair(_tab_status, "FPS", 60, sr2, SYS_COLOR);
    _tasks_val = create_stat_pair(_tab_status, "TASKS", 110, sr2, SYS_COLOR);
    _lvgl_objs_val = create_stat_pair(_tab_status, "LVGL", 170, sr2, SYS_COLOR);
    _flash_val = create_stat_pair(_tab_status, "FLASH", 230, sr2, SYS_COLOR);

    // ERRORS -- 32 is the 2-line stat-pair block height (TEMP row)
    int ey = sr2 + 32 + 14;
    create_section_header(_tab_status, "ERRORS", 0, ey);
    _err_count_lbl = lv_label_create(_tab_status);
    lv_label_set_text(_err_count_lbl, "(0)");
    lv_obj_set_style_text_font(_err_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_count_lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(_err_count_lbl, 70, ey);
    lv_obj_clear_flag(_err_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *clr_btn = lv_obj_create(_tab_status);
    lv_obj_set_size(clr_btn, 40, 22);
    lv_obj_set_pos(clr_btn, 120, ey - 2);
    lv_obj_set_style_bg_color(clr_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_bg_opa(clr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clr_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(clr_btn, 1, 0);
    lv_obj_set_style_radius(clr_btn, 4, 0);
    lv_obj_set_style_pad_all(clr_btn, 0, 0);
    lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(clr_btn, [](lv_event_t *e) {
        error_log_clear();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *clr_lbl = lv_label_create(clr_btn);
    lv_label_set_text(clr_lbl, "CLR");
    lv_obj_set_style_text_font(clr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clr_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_center(clr_lbl);

    _err_list_lbl = lv_label_create(_tab_status);
    lv_label_set_text(_err_list_lbl, "(none)");
    lv_obj_set_style_text_font(_err_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_list_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_set_pos(_err_list_lbl, 0, ey + 18);
    lv_obj_set_width(_err_list_lbl, 310);
    lv_obj_clear_flag(_err_list_lbl, LV_OBJ_FLAG_CLICKABLE);

    // FPS counter — increment each refresh, calculate every second
    _fps_last_time = millis();
    lv_timer_create([](lv_timer_t *t) {
        _frame_count++;
        uint32_t now = millis();
        if (now - _fps_last_time >= 1000) {
            _fps = (uint16_t)(_frame_count * 1000 / (now - _fps_last_time));
            _frame_count = 0;
            _fps_last_time = now;
        }
    }, 33, nullptr);

    status_tab_refresh(nullptr);
    lv_timer_create(status_tab_refresh, 2000, nullptr);

    // Display / Screensaver button -- deactivated 2026-07-23 along with the
    // rest of screensaver.cpp (see the #if 0 block there for why). Left
    // commented out rather than deleted so it's a one-step re-enable.
#if 0
    lv_obj_t *display_btn = lv_button_create(_panel);
    lv_obj_set_size(display_btn, FIELD_W, 40);
    lv_obj_set_pos(display_btn, 0, 300);
    lv_obj_set_style_bg_color(display_btn, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(display_btn, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(display_btn, 1, 0);
    lv_obj_set_style_radius(display_btn, 6, 0);
    { lv_obj_t *lbl = lv_label_create(display_btn);
      lv_label_set_text(lbl, "Display / Screensaver...");
      lv_obj_set_style_text_color(lbl, lv_color_hex(0xccccdd), 0);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
      lv_obj_center(lbl); }
    lv_obj_add_event_cb(display_btn, [](lv_event_t *e) {
        screensaver_show_settings();
    }, LV_EVENT_CLICKED, nullptr);
#endif

    // === Save button (centered at bottom) -- CONFIG tab only, see show_tab() ===
    _save_btn = lv_button_create(_panel);
    lv_obj_set_size(_save_btn, 120, 40);
    lv_obj_align(_save_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(_save_btn, ACCENT_COLOR, 0);
    lv_obj_set_style_radius(_save_btn, 8, 0);

    lv_obj_t *save_label = lv_label_create(_save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_16, 0);
    lv_obj_center(save_label);

    lv_obj_add_event_cb(_save_btn, save_and_close, LV_EVENT_CLICKED, nullptr);

    // Now that every widget show_tab() touches (_tab_config/_tab_status/
    // _save_btn/both tab buttons) actually exists, it's safe to call --
    // CONFIG shown by default whenever Settings is built.
    show_tab(0);

    // === On-screen keyboard (hidden by default) ===
    _keyboard = lv_keyboard_create(_overlay);
    lv_obj_set_size(_keyboard, LCD_H_RES, 200);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_CANCEL, nullptr);
}

void settings_show() {
    if (_visible) return;
    _visible = true;
    _shown_at_ms = millis();

    // Reload config in case it changed
    _cfg = storage_load_config();
    lv_textarea_set_text(_ta_ssid, _cfg.wifi_ssid);
    lv_textarea_set_text(_ta_pass, _cfg.wifi_pass);
    lv_textarea_set_password_mode(_ta_pass, true);
    lv_label_set_text(lv_obj_get_child(_btn_show_pass, 0), LV_SYMBOL_EYE_OPEN);

    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        lv_textarea_set_text(_ta_radius[i], rbuf);
    }

    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_metric, LV_STATE_CHECKED);

    if (_cfg.use_ethernet) lv_obj_add_state(_sw_ethernet, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_ethernet, LV_STATE_CHECKED);

    show_tab(0); // always reopen on CONFIG, regardless of which tab was showing last time

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
