#include <Arduino.h>
#include "detail_card.h"
#include "geo.h"
#include "../data/storage.h"
#include "../data/enrichment.h"
#include "../pins_config.h"

static lv_obj_t *_card = nullptr;

// Header
static lv_obj_t *_callsign_label = nullptr;
static lv_obj_t *_badge_label = nullptr;

// Sub-header
static lv_obj_t *_reg_label = nullptr;

// Identity
static lv_obj_t *_operator_label = nullptr;
static lv_obj_t *_type_label = nullptr;
static lv_obj_t *_cat_label = nullptr;
static lv_obj_t *_aircraft_detail_label = nullptr;
// Data grid row 1 — flight state
static lv_obj_t *_alt_label = nullptr;
static lv_obj_t *_spd_label = nullptr;
static lv_obj_t *_hdg_label = nullptr;
static lv_obj_t *_vrate_label = nullptr;
static lv_obj_t *_squawk_label = nullptr;
static lv_obj_t *_status_label = nullptr;

// Data grid row 2 — position & tracking
static lv_obj_t *_dist_label = nullptr;
static lv_obj_t *_bearing_label = nullptr;
static lv_obj_t *_lat_label = nullptr;
static lv_obj_t *_lon_label = nullptr;
static lv_obj_t *_track_label = nullptr;
static lv_obj_t *_signal_label = nullptr;

// Data grid row 3 — extended flight params
static lv_obj_t *_mach_label = nullptr;
static lv_obj_t *_ias_label = nullptr;
static lv_obj_t *_tas_label = nullptr;
static lv_obj_t *_nav_alt_label = nullptr;
static lv_obj_t *_roll_label = nullptr;
static lv_obj_t *_qnh_label = nullptr;

// Loading
static lv_obj_t *_loading_spinner = nullptr;

// Live update timer
static lv_timer_t *_update_timer = nullptr;

static bool _visible = false;
static Aircraft _current_ac;

#define CARD_H 330
#define CARD_BG lv_color_hex(0x141428)
#define CARD_TEXT lv_color_hex(0xccccdd)
#define CARD_ACCENT lv_color_hex(0x4488ff)
#define CARD_DIM lv_color_hex(0x666688)
#define CARD_HIGHLIGHT lv_color_hex(0x88bbff)

// Data grid column positions (6 cols across ~960px usable)
#define COL1 0
#define COL2 160
#define COL3 320
#define COL4 480
#define COL5 640
#define COL6 800

static const char *decode_category(const char *cat) {
    if (!cat[0]) return "";
    if (cat[0] == 'A') {
        switch (cat[1]) {
            case '0': return "Uncat";
            case '1': return "Light <15.5k lbs";
            case '2': return "Small 15.5-75k lbs";
            case '3': return "Large 75-300k lbs";
            case '4': return "High vortex (B757)";
            case '5': return "Heavy >300k lbs";
            case '6': return "High perf >5g, >400kt";
            case '7': return "Rotorcraft";
        }
    } else if (cat[0] == 'B') {
        switch (cat[1]) {
            case '1': return "Glider/Sailplane";
            case '2': return "Lighter-than-air";
            case '3': return "Parachutist";
            case '4': return "Ultralight";
            case '6': return "UAV/Drone";
            case '7': return "Space vehicle";
        }
    } else if (cat[0] == 'C') {
        switch (cat[1]) {
            case '1': return "Emergency vehicle";
            case '2': return "Service vehicle";
            case '3': return "Obstruction";
        }
    }
    return "";
}

static const char *decode_squawk(uint16_t sq) {
    switch (sq) {
        case 7500: return "HIJACK";
        case 7600: return "RADIO FAIL";
        case 7700: return "EMERGENCY";
        case 1200: return "VFR";
        case 1000: return "IFR";
        case 2000: return "IFR unassigned";
        case 7000: return "VFR (EU)";
        default: return "";
    }
}

static void on_enrichment_ready(AircraftEnrichment *data) {
    if (!_visible) return;

    // Model — more detailed than bulk API desc
    if (data->model[0]) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (%s)", data->model, _current_ac.type_code);
        lv_label_set_text(_type_label, buf);
    }

    // Aircraft details line: manufacturer | owner | country year | engines
    char detail[160] = {};
    int pos = 0;
    if (data->manufacturer[0]) {
        pos += snprintf(detail + pos, sizeof(detail) - pos, "%s", data->manufacturer);
    }
    if (data->owner[0]) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, "  |  ");
        pos += snprintf(detail + pos, sizeof(detail) - pos, "%s", data->owner);
    }
    if (data->registered_country[0]) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, "  |  ");
        pos += snprintf(detail + pos, sizeof(detail) - pos, "%s", data->registered_country);
        if (data->year_built > 0) {
            pos += snprintf(detail + pos, sizeof(detail) - pos, " %d", data->year_built);
        }
    } else if (data->year_built > 0) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, "  |  ");
        pos += snprintf(detail + pos, sizeof(detail) - pos, "Built %d", data->year_built);
    }
    if (data->engine_count > 0 && data->engine_type[0]) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, "  |  ");
        snprintf(detail + pos, sizeof(detail) - pos, "%dx %s", data->engine_count, data->engine_type);
    }
    if (detail[0]) lv_label_set_text(_aircraft_detail_label, detail);

    // Hide spinner when fully loaded
    if (data->loaded && _loading_spinner) {
        lv_obj_add_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_data_row(lv_obj_t *parent, const char *label_text,
                                int x, int y, lv_obj_t **value_label) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, CARD_DIM, 0);
    lv_obj_set_pos(lbl, x, y);

    *value_label = lv_label_create(parent);
    lv_label_set_text(*value_label, "--");
    lv_obj_set_style_text_font(*value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(*value_label, CARD_TEXT, 0);
    lv_obj_set_pos(*value_label, x, y + 16);

    return lbl;
}

static void update_timer_cb(lv_timer_t *timer) {
    if (!_visible) return;

    // Update signal age
    uint32_t age_ms = millis() - _current_ac.last_seen;
    if (age_ms < 60000) {
        lv_label_set_text_fmt(_signal_label, "%lus", (unsigned long)(age_ms / 1000));
    } else {
        lv_label_set_text_fmt(_signal_label, "%lum%lus",
            (unsigned long)(age_ms / 60000), (unsigned long)((age_ms / 1000) % 60));
    }

    // Update tracked time
    if (_current_ac.trail_count >= 2) {
        uint32_t dur_ms = _current_ac.trail[_current_ac.trail_count - 1].timestamp
                          - _current_ac.trail[0].timestamp;
        uint32_t secs = dur_ms / 1000;
        if (secs < 60) {
            lv_label_set_text_fmt(_track_label, "%d pts %lus",
                _current_ac.trail_count, (unsigned long)secs);
        } else {
            lv_label_set_text_fmt(_track_label, "%d pts %lum%lus",
                _current_ac.trail_count, (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        }
    }
}

void detail_card_init(lv_obj_t *parent) {
    _card = lv_obj_create(parent);
    lv_obj_set_size(_card, LCD_H_RES, CARD_H);
    lv_obj_set_pos(_card, 0, LCD_V_RES); // start off-screen (below)
    lv_obj_set_style_bg_color(_card, CARD_BG, 0);
    lv_obj_set_style_bg_opa(_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_card, 0, 0);
    lv_obj_set_style_radius(_card, 12, 0);
    lv_obj_set_style_pad_all(_card, 16, 0);
    lv_obj_set_style_clip_corner(_card, true, 0);
    lv_obj_add_flag(_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(_card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_card, LV_SCROLLBAR_MODE_AUTO);

    // Drag handle indicator
    lv_obj_t *handle = lv_obj_create(_card);
    lv_obj_set_size(handle, 40, 4);
    lv_obj_set_style_bg_color(handle, CARD_DIM, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -8);

    // === HEADER ===
    _callsign_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_callsign_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_callsign_label, lv_color_white(), 0);
    lv_obj_set_pos(_callsign_label, 0, 4);

    _badge_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_badge_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_badge_label, 280, 12);
    lv_label_set_text(_badge_label, "");

    _loading_spinner = lv_spinner_create(_card);
    lv_obj_set_size(_loading_spinner, 24, 24);
    lv_obj_set_pos(_loading_spinner, 940, 4);
    lv_obj_add_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);

    // Sub-header: Reg | ICAO | Squawk
    _reg_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_reg_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_reg_label, CARD_DIM, 0);
    lv_obj_set_pos(_reg_label, 0, 36);

    // === IDENTITY ===
    _operator_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_operator_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_operator_label, CARD_ACCENT, 0);
    lv_obj_set_pos(_operator_label, 0, 58);

    _type_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_type_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_type_label, CARD_TEXT, 0);
    lv_obj_set_pos(_type_label, 0, 78);

    _cat_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_cat_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_cat_label, CARD_DIM, 0);
    lv_obj_set_pos(_cat_label, 500, 78);

    _aircraft_detail_label = lv_label_create(_card);
    lv_label_set_text(_aircraft_detail_label, "");
    lv_obj_set_style_text_font(_aircraft_detail_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_aircraft_detail_label, CARD_DIM, 0);
    lv_obj_set_pos(_aircraft_detail_label, 0, 98);

    // === DATA GRID ROW 1 — flight state ===
    int y1 = 178;
    make_data_row(_card, "ALTITUDE", COL1, y1, &_alt_label);
    make_data_row(_card, "GND SPD", COL2, y1, &_spd_label);
    make_data_row(_card, "HEADING", COL3, y1, &_hdg_label);
    make_data_row(_card, "V/S", COL4, y1, &_vrate_label);
    make_data_row(_card, "SQUAWK", COL5, y1, &_squawk_label);
    make_data_row(_card, "STATUS", COL6, y1, &_status_label);

    // === DATA GRID ROW 2 — position & tracking ===
    int y2 = 220;
    make_data_row(_card, "DISTANCE", COL1, y2, &_dist_label);
    make_data_row(_card, "BEARING", COL2, y2, &_bearing_label);
    make_data_row(_card, "LAT", COL3, y2, &_lat_label);
    make_data_row(_card, "LON", COL4, y2, &_lon_label);
    make_data_row(_card, "TRACKED", COL5, y2, &_track_label);
    make_data_row(_card, "SIGNAL", COL6, y2, &_signal_label);

    // === DATA GRID ROW 3 — extended flight params ===
    int y3 = 262;
    make_data_row(_card, "MACH", COL1, y3, &_mach_label);
    make_data_row(_card, "IAS", COL2, y3, &_ias_label);
    make_data_row(_card, "TAS", COL3, y3, &_tas_label);
    make_data_row(_card, "NAV ALT", COL4, y3, &_nav_alt_label);
    make_data_row(_card, "ROLL", COL5, y3, &_roll_label);
    make_data_row(_card, "QNH", COL6, y3, &_qnh_label);

    // Tap to close
    lv_obj_add_event_cb(_card, [](lv_event_t *e) {
        detail_card_hide();
    }, LV_EVENT_CLICKED, nullptr);

    // Live update timer (1s) — starts paused
    _update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    lv_timer_pause(_update_timer);

    _visible = false;
}

void detail_card_show(const Aircraft *ac) {
    memcpy(&_current_ac, ac, sizeof(Aircraft));

    // === HEADER ===
    lv_label_set_text(_callsign_label, ac->callsign[0] ? ac->callsign : ac->icao_hex);

    // Military / Emergency badge
    if (ac->is_emergency) {
        const char *emg = ac->squawk == 7500 ? "HIJACK" : ac->squawk == 7600 ? "RADIO FAIL" : "EMERGENCY";
        lv_label_set_text(_badge_label, emg);
        lv_obj_set_style_text_color(_badge_label, lv_color_hex(0xff3333), 0);
    } else if (ac->is_military) {
        lv_label_set_text(_badge_label, "MILITARY");
        lv_obj_set_style_text_color(_badge_label, lv_color_hex(0xffaa00), 0);
    } else {
        lv_label_set_text(_badge_label, "");
    }

    // Sub-header: Reg | ICAO | Squawk with decode
    const char *sq_decode = decode_squawk(ac->squawk);
    if (sq_decode[0]) {
        lv_label_set_text_fmt(_reg_label, "%s  |  %s  |  Sq %04d (%s)",
                              ac->registration, ac->icao_hex, ac->squawk, sq_decode);
    } else {
        lv_label_set_text_fmt(_reg_label, "%s  |  %s  |  Sq %04d",
                              ac->registration, ac->icao_hex, ac->squawk);
    }

    // === IDENTITY ===
    lv_label_set_text(_operator_label, ac->owner_op[0] ? ac->owner_op : "");

    // Type description
    if (ac->desc[0]) {
        char buf[52];
        snprintf(buf, sizeof(buf), "%s (%s)", ac->desc, ac->type_code);
        lv_label_set_text(_type_label, buf);
    } else if (ac->type_code[0]) {
        lv_label_set_text(_type_label, ac->type_code);
    } else {
        lv_label_set_text(_type_label, "");
    }

    // Category
    const char *cat_desc = decode_category(ac->category);
    if (cat_desc[0]) {
        lv_label_set_text_fmt(_cat_label, "Cat %s: %s", ac->category, cat_desc);
    } else if (ac->category[0]) {
        lv_label_set_text_fmt(_cat_label, "Cat %s", ac->category);
    } else {
        lv_label_set_text(_cat_label, "");
    }

    // Clear enrichment fields
    lv_label_set_text(_aircraft_detail_label, "");
    // === DATA GRID ROW 1 — flight state ===
    if (ac->on_ground) {
        lv_label_set_text(_alt_label, "GND");
    } else if (ac->altitude >= 18000) {
        lv_label_set_text_fmt(_alt_label, "FL%d", ac->altitude / 100);
    } else {
        lv_label_set_text_fmt(_alt_label, "%d ft", ac->altitude);
    }

    lv_label_set_text_fmt(_spd_label, "%d kts", ac->speed);
    lv_label_set_text_fmt(_hdg_label, "%03d", ac->heading);
    lv_label_set_text_fmt(_vrate_label, "%+d fpm", ac->vert_rate);
    lv_label_set_text_fmt(_squawk_label, "%04d", ac->squawk);

    const char *status;
    if (ac->on_ground) status = "On Ground";
    else if (ac->vert_rate > 300) status = "Climbing";
    else if (ac->vert_rate < -300) status = "Descending";
    else status = "Cruising";
    lv_label_set_text(_status_label, status);

    // === DATA GRID ROW 2 — position & tracking ===
    float dist = MapProjection::distance_nm(g_config.home_lat, g_config.home_lon, ac->lat, ac->lon);
    lv_label_set_text_fmt(_dist_label, "%.1f nm", dist);

    float dlon = (ac->lon - g_config.home_lon) * M_PI / 180.0f;
    float y = sinf(dlon) * cosf(ac->lat * M_PI / 180.0f);
    float x = cosf(g_config.home_lat * M_PI / 180.0f) * sinf(ac->lat * M_PI / 180.0f) -
              sinf(g_config.home_lat * M_PI / 180.0f) * cosf(ac->lat * M_PI / 180.0f) * cosf(dlon);
    int bearing = (int)(atan2f(y, x) * 180.0f / M_PI + 360.0f) % 360;
    lv_label_set_text_fmt(_bearing_label, "%03d", bearing);

    lv_label_set_text_fmt(_lat_label, "%.4f", ac->lat);
    lv_label_set_text_fmt(_lon_label, "%.4f", ac->lon);

    // Trail stats
    if (ac->trail_count >= 2) {
        uint32_t dur_ms = ac->trail[ac->trail_count - 1].timestamp - ac->trail[0].timestamp;
        uint32_t secs = dur_ms / 1000;
        if (secs < 60) {
            lv_label_set_text_fmt(_track_label, "%d pts %lus",
                ac->trail_count, (unsigned long)secs);
        } else {
            lv_label_set_text_fmt(_track_label, "%d pts %lum%lus",
                ac->trail_count, (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        }
    } else if (ac->trail_count == 1) {
        lv_label_set_text(_track_label, "1 pt");
    } else {
        lv_label_set_text(_track_label, "new");
    }

    // Signal age
    uint32_t age_ms = millis() - ac->last_seen;
    if (age_ms < 1000) {
        lv_label_set_text(_signal_label, "live");
    } else {
        lv_label_set_text_fmt(_signal_label, "%lus", (unsigned long)(age_ms / 1000));
    }

    // === DATA GRID ROW 3 — extended flight params ===
    if (ac->mach > 0.01f) {
        lv_label_set_text_fmt(_mach_label, "%.3f", ac->mach);
    } else {
        lv_label_set_text(_mach_label, "--");
    }

    if (ac->ias > 0) {
        lv_label_set_text_fmt(_ias_label, "%d kts", ac->ias);
    } else {
        lv_label_set_text(_ias_label, "--");
    }

    if (ac->tas > 0) {
        lv_label_set_text_fmt(_tas_label, "%d kts", ac->tas);
    } else {
        lv_label_set_text(_tas_label, "--");
    }

    if (ac->nav_altitude > 0) {
        if (ac->nav_altitude >= 18000) {
            lv_label_set_text_fmt(_nav_alt_label, "FL%d", ac->nav_altitude / 100);
        } else {
            lv_label_set_text_fmt(_nav_alt_label, "%d ft", ac->nav_altitude);
        }
    } else {
        lv_label_set_text(_nav_alt_label, "--");
    }

    // Roll angle
    if (ac->roll != 0.0f) {
        lv_label_set_text_fmt(_roll_label, "%.1f%s", ac->roll, ac->roll > 0 ? " R" : " L");
    } else {
        lv_label_set_text(_roll_label, "--");
    }

    // Altimeter QNH
    if (ac->nav_qnh > 0.0f) {
        lv_label_set_text_fmt(_qnh_label, "%.1f hPa", ac->nav_qnh);
    } else {
        lv_label_set_text(_qnh_label, "--");
    }

    // === Slide in ===
    lv_obj_scroll_to_y(_card, 0, LV_ANIM_OFF);
    _visible = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _card);
    lv_anim_set_values(&a, LCD_V_RES, LCD_V_RES - CARD_H);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_y((lv_obj_t *)obj, v);
    });
    lv_anim_start(&a);

    // Start live update timer
    lv_timer_resume(_update_timer);

    // Fetch enrichment (adsbdb + planespotters)
    lv_obj_clear_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);
    enrichment_fetch(ac->icao_hex, ac->registration, on_enrichment_ready);
}

void detail_card_hide() {
    if (!_visible) return;
    _visible = false;

    // Pause live updates
    lv_timer_pause(_update_timer);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _card);
    lv_anim_set_values(&a, lv_obj_get_y(_card), LCD_V_RES);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_y((lv_obj_t *)obj, v);
    });
    lv_anim_start(&a);
}

bool detail_card_is_visible() {
    return _visible;
}

void detail_card_scroll(int delta) {
    if (!_visible || !_card) return;
    lv_obj_scroll_by(_card, 0, -delta * 40, LV_ANIM_ON);
}
