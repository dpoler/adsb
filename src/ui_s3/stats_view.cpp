#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include "stats_view.h"
#include "../ui/stats.h"
#include "views.h"
#include "../pins_s3.h"
#include "../data/fetcher.h"
#include "../data/error_log.h"
#include "../ui/geo.h" // altitude_color()

#define STATS_W LCD_H_RES
#define STATS_H (LCD_V_RES - 24)
#define BG_COLOR lv_color_hex(0x0a0a1a)
#define DIM_COLOR lv_color_hex(0x666688)
#define ACCENT_COLOR lv_color_hex(0x4488ff)
#define SYS_COLOR lv_color_hex(0x44cc88)
#define WARN_COLOR lv_color_hex(0xccaa00)

static AircraftList *_list = nullptr;
static lv_obj_t *_container = nullptr;

// Current count
static lv_obj_t *_count_label = nullptr;

// Category rows
struct BarRow {
    lv_obj_t *name_lbl;
    lv_obj_t *count_lbl;
    lv_obj_t *bar;
};

static BarRow _cat_rows[5];
static const char *CAT_NAMES[] = {"JETS", "GA", "HELI", "MIL", "EMRG"};
static const uint32_t CAT_COLORS[] = {0x4488ff, 0x88aacc, 0x44ddaa, 0xffaa00, 0xff3333};

// Altitude rows -- colors sourced from altitude_color() (geo.h) at a
// representative altitude in each band, rather than a second hardcoded
// palette, so this can't drift from what the trails on Map/Radar draw.
static BarRow _alt_rows[6];
static const char *ALT_NAMES[] = {"GND", "<5k", "<15k", "<25k", "<35k", "35k+"};
static const int32_t ALT_SAMPLES[] = {0, 2500, 10000, 20000, 30000, 45000};

// Records
static lv_obj_t *_fastest_val = nullptr;
static lv_obj_t *_slowest_val = nullptr;
static lv_obj_t *_highest_val = nullptr;
static lv_obj_t *_lowest_val = nullptr;
static lv_obj_t *_closest_val = nullptr;

// Session stats
static lv_obj_t *_unique_val = nullptr;
static lv_obj_t *_peak_val = nullptr;
static lv_obj_t *_uptime_val = nullptr;

// System health
static lv_obj_t *_heap_val = nullptr;
static lv_obj_t *_psram_val = nullptr;
static lv_obj_t *_temp_val = nullptr;

// Network stats
static lv_obj_t *_ip_val = nullptr;
static lv_obj_t *_fetch_val = nullptr;
static lv_obj_t *_rssi_val = nullptr;

// Error log
static lv_obj_t *_err_count_lbl = nullptr;
static lv_obj_t *_err_list_lbl = nullptr;

#define BAR_MAX_W 100
#define BAR_H 12

static lv_obj_t *create_bar(lv_obj_t *parent, int x, int y, lv_color_t color) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 0, BAR_H);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static void create_bar_row(lv_obj_t *parent, BarRow *row, const char *name,
                           uint32_t color_hex, int x, int y) {
    lv_color_t color = lv_color_hex(color_hex);

    row->name_lbl = lv_label_create(parent);
    lv_label_set_text(row->name_lbl, name);
    lv_obj_set_style_text_font(row->name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(row->name_lbl, color, 0);
    lv_obj_set_pos(row->name_lbl, x, y + 1);
    lv_obj_clear_flag(row->name_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->count_lbl = lv_label_create(parent);
    lv_label_set_text(row->count_lbl, "0");
    lv_obj_set_style_text_font(row->count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(row->count_lbl, lv_color_hex(0xccccdd), 0);
    lv_obj_set_pos(row->count_lbl, x + 38, y + 1);
    lv_obj_clear_flag(row->count_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->bar = create_bar(parent, x + 60, y, color);
}

static void update_bar(BarRow *row, int count, int total) {
    lv_label_set_text_fmt(row->count_lbl, "%d", count);
    int w = (total > 0) ? (count * BAR_MAX_W / total) : 0;
    if (w < 2 && count > 0) w = 2;
    lv_obj_set_width(row->bar, w);
}

static lv_obj_t *create_stat_pair(lv_obj_t *parent, const char *header, int x, int y,
                                   lv_color_t val_color = lv_color_hex(0x44cc88)) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, header);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
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

static void create_section_header(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void refresh_stats(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_STATS) return;

    stats_update(_list);
    const SessionStats *s = stats_get();
    const FetcherStats *fs = fetcher_get_stats();

    lv_label_set_text_fmt(_count_label, "%d", s->current_count);

    // Category bars
    int cat_counts[] = {s->jets, s->ga, s->heli, s->military, s->emergency};
    int cat_total = s->current_count > 0 ? s->current_count : 1;
    for (int i = 0; i < 5; i++) {
        update_bar(&_cat_rows[i], cat_counts[i], cat_total);
    }

    // Altitude bars
    int alt_counts[] = {s->alt_gnd, s->alt_low, s->alt_med_low,
                        s->alt_med, s->alt_high, s->alt_very_high};
    int alt_max = 1;
    for (int i = 0; i < 6; i++) {
        if (alt_counts[i] > alt_max) alt_max = alt_counts[i];
    }
    for (int i = 0; i < 6; i++) {
        update_bar(&_alt_rows[i], alt_counts[i], alt_max);
    }

    // Records
    if (s->fastest_callsign[0]) {
        lv_label_set_text_fmt(_fastest_val, "%s %dkt", s->fastest_callsign, s->fastest_speed);
    } else {
        lv_label_set_text(_fastest_val, "--");
    }
    if (s->slowest_callsign[0] && s->slowest_speed < 99999) {
        lv_label_set_text_fmt(_slowest_val, "%s %dkt", s->slowest_callsign, s->slowest_speed);
    } else {
        lv_label_set_text(_slowest_val, "--");
    }
    if (s->highest_callsign[0] && s->highest_alt > -9999) {
        if (s->highest_alt >= 18000) {
            lv_label_set_text_fmt(_highest_val, "%s FL%d", s->highest_callsign, s->highest_alt / 100);
        } else {
            lv_label_set_text_fmt(_highest_val, "%s %dft", s->highest_callsign, s->highest_alt);
        }
    } else {
        lv_label_set_text(_highest_val, "--");
    }
    if (s->lowest_callsign[0] && s->lowest_alt < 999999) {
        lv_label_set_text_fmt(_lowest_val, "%s %dft", s->lowest_callsign, s->lowest_alt);
    } else {
        lv_label_set_text(_lowest_val, "--");
    }
    if (s->closest_callsign[0] && s->closest_dist < 9999.0f) {
        lv_label_set_text_fmt(_closest_val, "%s %.1fnm", s->closest_callsign, (double)s->closest_dist);
    } else {
        lv_label_set_text(_closest_val, "--");
    }

    // Session
    lv_label_set_text_fmt(_unique_val, "%d", s->unique_seen);
    lv_label_set_text_fmt(_peak_val, "%d", s->peak_count);

    uint32_t uptime_s = (millis() - s->boot_time) / 1000;
    int hrs = uptime_s / 3600;
    int mins = (uptime_s % 3600) / 60;
    int secs = uptime_s % 60;
    lv_label_set_text_fmt(_uptime_val, "%02d:%02d:%02d", hrs, mins, secs);

    // System health
    uint32_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    lv_label_set_text_fmt(_heap_val, "%luK/%luKmin", (unsigned long)(heap_free / 1024), (unsigned long)(heap_min / 1024));
    lv_label_set_text_fmt(_psram_val, "%.1fM", (double)psram_free / (1024.0 * 1024.0));

    float temp = temperatureRead();
    if (temp > 0) {
        lv_color_t tc = temp > 70 ? lv_color_hex(0xff3333) : temp > 55 ? WARN_COLOR : SYS_COLOR;
        lv_obj_set_style_text_color(_temp_val, tc, 0);
        lv_label_set_text_fmt(_temp_val, "%.0fC", (double)temp);
    } else {
        lv_label_set_text(_temp_val, "N/A");
    }

    // Network
    if (fs->ip_addr[0]) {
        lv_label_set_text(_ip_val, fs->ip_addr);
    }
    lv_label_set_text_fmt(_fetch_val, "%lu/%lu", (unsigned long)fs->fetch_ok, (unsigned long)fs->fetch_fail);

    NetType net = fetcher_connection_type();
    if (net == NET_WIFI) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            lv_label_set_text_fmt(_rssi_val, "%d dBm", ap_info.rssi);
        } else {
            lv_label_set_text(_rssi_val, "WiFi --");
        }
    } else {
        lv_label_set_text(_rssi_val, "--");
    }

    // Error log (3 entries max)
    uint32_t err_total = error_log_total_count();
    lv_label_set_text_fmt(_err_count_lbl, "ERRORS (%lu)", (unsigned long)err_total);

    ErrorSnapshot snap = error_log_snapshot();
    if (snap.count == 0) {
        lv_label_set_text(_err_list_lbl, "(none)");
    } else {
        static char err_buf[256];
        int pos = 0;
        uint32_t now_ms = millis();
        int show = snap.count > 3 ? 3 : snap.count;
        for (int i = snap.count - 1; i >= snap.count - show && pos < (int)sizeof(err_buf) - 60; i--) {
            uint32_t age_s = (now_ms - snap.entries[i].timestamp) / 1000;
            int m = age_s / 60;
            int sec = age_s % 60;
            pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
                "%dm%02ds %s\n", m, sec, snap.entries[i].msg);
        }
        if (pos > 0) err_buf[pos - 1] = '\0';
        lv_label_set_text(_err_list_lbl, err_buf);
    }
}

void stats_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    // Scrollable container for single-column layout
    _container = lv_obj_create(parent);
    lv_obj_set_size(_container, STATS_W, STATS_H);
    lv_obj_set_pos(_container, 0, 0);
    lv_obj_set_style_bg_color(_container, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_radius(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 0, 0);
    lv_obj_add_flag(_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLL_CHAIN);

    // Two-column layout for 480px: left (x=10) and right (x=250)
    int lx = 10;
    int rx = 250;

    // === LEFT COLUMN ===

    // Current count
    _count_label = lv_label_create(_container);
    lv_label_set_text(_count_label, "0");
    lv_obj_set_style_text_font(_count_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_count_label, lv_color_white(), 0);
    lv_obj_set_pos(_count_label, lx, 6);
    lv_obj_clear_flag(_count_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *subtitle = lv_label_create(_container);
    lv_label_set_text(subtitle, "TRACKING");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(subtitle, DIM_COLOR, 0);
    lv_obj_set_pos(subtitle, lx, 30);
    lv_obj_clear_flag(subtitle, LV_OBJ_FLAG_CLICKABLE);

    // Category breakdown
    int cat_y = 48;
    for (int i = 0; i < 5; i++) {
        create_bar_row(_container, &_cat_rows[i], CAT_NAMES[i], CAT_COLORS[i],
                       lx, cat_y + i * 18);
    }

    // Records
    int rc_y = cat_y + 5 * 18 + 6;
    create_section_header(_container, "RECORDS", lx, rc_y);

    lv_color_t rec_hdr = DIM_COLOR;
    int rr = rc_y + 16;
    int rh = 16;

    auto make_rec_row = [&](const char *hdr, int y) -> lv_obj_t* {
        lv_obj_t *h = lv_label_create(_container);
        lv_label_set_text(h, hdr);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, rec_hdr, 0);
        lv_obj_set_pos(h, lx, y);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *v = lv_label_create(_container);
        lv_label_set_text(v, "--");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(0xccccdd), 0);
        lv_obj_set_pos(v, lx + 60, y);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        return v;
    };

    _fastest_val = make_rec_row("FAST", rr);
    lv_obj_set_style_text_color(_fastest_val, lv_color_hex(0xff66cc), 0);
    _slowest_val = make_rec_row("SLOW", rr + rh);
    lv_obj_set_style_text_color(_slowest_val, lv_color_hex(0x66aaff), 0);
    _highest_val = make_rec_row("HIGH", rr + rh * 2);
    lv_obj_set_style_text_color(_highest_val, altitude_color(45000), 0); // high end of the altitude gradient
    _lowest_val  = make_rec_row("LOW",  rr + rh * 3);
    lv_obj_set_style_text_color(_lowest_val, altitude_color(1000), 0); // low end of the altitude gradient
    _closest_val = make_rec_row("NEAR", rr + rh * 4);
    lv_obj_set_style_text_color(_closest_val, lv_color_hex(0x44ddaa), 0);

    // === RIGHT COLUMN ===

    // Altitude distribution
    create_section_header(_container, "ALTITUDE", rx, 6);
    for (int i = 0; i < 6; i++) {
        create_bar_row(_container, &_alt_rows[i], ALT_NAMES[i],
                       lv_color_to_u32(altitude_color(ALT_SAMPLES[i])),
                       rx, 22 + i * 18);
    }

    // Session
    int ss_y = 22 + 6 * 18 + 6;
    create_section_header(_container, "SESSION", rx, ss_y);
    _unique_val = create_stat_pair(_container, "UNIQUE", rx, ss_y + 16, ACCENT_COLOR);
    _peak_val = create_stat_pair(_container, "PEAK", rx + 70, ss_y + 16, ACCENT_COLOR);
    _uptime_val = create_stat_pair(_container, "UPTIME", rx + 130, ss_y + 16, ACCENT_COLOR);

    // System health
    int sy = ss_y + 52;
    create_section_header(_container, "SYSTEM", rx, sy);
    _heap_val = create_stat_pair(_container, "HEAP", rx, sy + 16, SYS_COLOR);
    _psram_val = create_stat_pair(_container, "PSRAM", rx + 110, sy + 16, SYS_COLOR);
    _temp_val = create_stat_pair(_container, "TEMP", rx, sy + 50, SYS_COLOR);

    // Network
    int ny = sy + 84;
    create_section_header(_container, "NETWORK", rx, ny);
    _ip_val = create_stat_pair(_container, "IP", rx, ny + 16, SYS_COLOR);
    _fetch_val = create_stat_pair(_container, "OK/ERR", rx + 110, ny + 16, SYS_COLOR);
    _rssi_val = create_stat_pair(_container, "SIGNAL", rx, ny + 50, SYS_COLOR);

    // Error log (3 entries max)
    int ey = ny + 84;
    _err_count_lbl = lv_label_create(_container);
    lv_label_set_text(_err_count_lbl, "ERRORS (0)");
    lv_obj_set_style_text_font(_err_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_count_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(_err_count_lbl, rx, ey);
    lv_obj_clear_flag(_err_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    // CLR button
    lv_obj_t *clr_btn = lv_obj_create(_container);
    lv_obj_set_size(clr_btn, 36, 18);
    lv_obj_set_pos(clr_btn, rx + 100, ey);
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

    _err_list_lbl = lv_label_create(_container);
    lv_label_set_text(_err_list_lbl, "(none)");
    lv_obj_set_style_text_font(_err_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_list_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_set_pos(_err_list_lbl, rx, ey + 18);
    lv_obj_set_width(_err_list_lbl, 220);
    lv_obj_clear_flag(_err_list_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Refresh timer
    lv_timer_create(refresh_stats, 2000, nullptr);
}

void stats_view_update() {
    // triggered externally if needed
}
