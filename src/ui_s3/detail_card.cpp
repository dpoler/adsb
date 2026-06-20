#include <Arduino.h>
#include "detail_card.h"
#include "../ui/geo.h"
#include "../data/storage.h"

#include "../data/enrichment.h"
#include "../pins_s3.h"

static lv_obj_t *_card = nullptr;

// Header
static lv_obj_t *_callsign_label = nullptr;
static lv_obj_t *_badge_label = nullptr;

// Sub-header
static lv_obj_t *_reg_label = nullptr;

// Identity
static lv_obj_t *_operator_label = nullptr;
static lv_obj_t *_route_label = nullptr;
static lv_obj_t *_type_label = nullptr;

// Data grid — 3 columns (ALT/SPD/HDG, VS/SQK/STATUS, DIST/BRG/SIGNAL)
static lv_obj_t *_alt_label = nullptr;
static lv_obj_t *_spd_label = nullptr;
static lv_obj_t *_hdg_label = nullptr;
static lv_obj_t *_vrate_label = nullptr;
static lv_obj_t *_squawk_label = nullptr;
static lv_obj_t *_status_label = nullptr;
static lv_obj_t *_dist_label = nullptr;
static lv_obj_t *_bearing_label = nullptr;
static lv_obj_t *_signal_label = nullptr;

// Loading
static lv_obj_t *_loading_spinner = nullptr;

// Live update timer
static lv_timer_t *_update_timer = nullptr;

static bool _visible = false;
static Aircraft _current_ac;

#define CARD_H 200
#define CARD_BG lv_color_hex(0x141428)
#define CARD_TEXT lv_color_hex(0xccccdd)
#define CARD_ACCENT lv_color_hex(0x4488ff)
#define CARD_DIM lv_color_hex(0x666688)

// 3 columns across ~448px (480 - 2*16 padding)
#define COL1 0
#define COL2 150
#define COL3 300

static const char *decode_squawk(uint16_t sq) {
    switch (sq) {
        case 7500: return "HIJACK";
        case 7600: return "RADIO FAIL";
        case 7700: return "EMERGENCY";
        case 1200: return "VFR";
        default: return "";
    }
}

static void on_enrichment_ready(AircraftEnrichment *data) {
    if (!_visible) return;

    if (data->airline[0]) {
        lv_label_set_text(_operator_label, data->airline);
    }

    if (data->model[0]) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%s)", data->model, _current_ac.type_code);
        lv_label_set_text(_type_label, buf);
    }

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
    lv_obj_set_style_text_font(*value_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(*value_label, CARD_TEXT, 0);
    lv_obj_set_pos(*value_label, x, y + 14);

    return lbl;
}

static void update_timer_cb(lv_timer_t *timer) {
    if (!_visible) return;

    uint32_t age_ms = millis() - _current_ac.last_seen;
    if (age_ms < 60000) {
        lv_label_set_text_fmt(_signal_label, "%lus", (unsigned long)(age_ms / 1000));
    } else {
        lv_label_set_text_fmt(_signal_label, "%lum%lus",
            (unsigned long)(age_ms / 60000), (unsigned long)((age_ms / 1000) % 60));
    }
}

void detail_card_init(lv_obj_t *parent) {
    _card = lv_obj_create(parent);
    lv_obj_set_size(_card, LCD_H_RES, CARD_H);
    lv_obj_set_pos(_card, 0, LCD_V_RES); // off-screen below
    lv_obj_set_style_bg_color(_card, CARD_BG, 0);
    lv_obj_set_style_bg_opa(_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_card, 0, 0);
    lv_obj_set_style_radius(_card, 8, 0);
    lv_obj_set_style_pad_all(_card, 12, 0);
    lv_obj_set_style_clip_corner(_card, true, 0);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLL_CHAIN);

    // Drag handle
    lv_obj_t *handle = lv_obj_create(_card);
    lv_obj_set_size(handle, 30, 3);
    lv_obj_set_style_bg_color(handle, CARD_DIM, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -6);

    // Header — callsign (montserrat_20 for S3)
    _callsign_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_callsign_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_callsign_label, lv_color_white(), 0);
    lv_obj_set_pos(_callsign_label, 0, 2);

    _badge_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_badge_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_badge_label, 200, 6);
    lv_label_set_text(_badge_label, "");

    _loading_spinner = lv_spinner_create(_card);
    lv_obj_set_size(_loading_spinner, 20, 20);
    lv_obj_set_pos(_loading_spinner, 430, 2);
    lv_obj_add_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);

    // Sub-header: Reg | ICAO
    _reg_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_reg_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_reg_label, CARD_DIM, 0);
    lv_obj_set_pos(_reg_label, 0, 26);

    // Identity
    _operator_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_operator_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_operator_label, CARD_ACCENT, 0);
    lv_obj_set_pos(_operator_label, 0, 42);

    _route_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_route_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_route_label, lv_color_white(), 0);
    lv_obj_set_pos(_route_label, 200, 42);

    _type_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_type_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_type_label, CARD_TEXT, 0);
    lv_obj_set_pos(_type_label, 0, 58);

    // Data grid row 1: ALT / SPD / HDG
    int y1 = 80;
    make_data_row(_card, "ALT", COL1, y1, &_alt_label);
    make_data_row(_card, "SPD", COL2, y1, &_spd_label);
    make_data_row(_card, "HDG", COL3, y1, &_hdg_label);

    // Data grid row 2: V/S / SQK / STATUS
    int y2 = 114;
    make_data_row(_card, "V/S", COL1, y2, &_vrate_label);
    make_data_row(_card, "SQK", COL2, y2, &_squawk_label);
    make_data_row(_card, "STATUS", COL3, y2, &_status_label);

    // Data grid row 3: DIST / BRG / SIGNAL
    int y3 = 148;
    make_data_row(_card, "DIST", COL1, y3, &_dist_label);
    make_data_row(_card, "BRG", COL2, y3, &_bearing_label);
    make_data_row(_card, "SIGNAL", COL3, y3, &_signal_label);

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

    // Header
    lv_label_set_text(_callsign_label, ac->callsign[0] ? ac->callsign : ac->icao_hex);

    // Badge
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

    // Sub-header
    const char *sq_decode = decode_squawk(ac->squawk);
    if (sq_decode[0]) {
        lv_label_set_text_fmt(_reg_label, "%s | %s | Sq%04d(%s)",
                              ac->registration, ac->icao_hex, ac->squawk, sq_decode);
    } else {
        lv_label_set_text_fmt(_reg_label, "%s | %s | Sq%04d",
                              ac->registration, ac->icao_hex, ac->squawk);
    }

    // Identity
    lv_label_set_text(_operator_label, ac->owner_op[0] ? ac->owner_op : "");

    if (ac->origin[0] && ac->origin[0] != '-' && ac->dest[0] && ac->dest[0] != '-') {
        lv_label_set_text_fmt(_route_label, "%s->%s", ac->origin, ac->dest);
    } else {
        lv_label_set_text(_route_label, "");
    }

    if (ac->desc[0]) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s (%s)", ac->desc, ac->type_code);
        lv_label_set_text(_type_label, buf);
    } else if (ac->type_code[0]) {
        lv_label_set_text(_type_label, ac->type_code);
    } else {
        lv_label_set_text(_type_label, "");
    }

    // Row 1: ALT / SPD / HDG
    if (ac->on_ground) {
        lv_label_set_text(_alt_label, "GND");
    } else if (ac->altitude >= 18000) {
        lv_label_set_text_fmt(_alt_label, "FL%d", ac->altitude / 100);
    } else {
        lv_label_set_text_fmt(_alt_label, "%d ft", ac->altitude);
    }
    lv_label_set_text_fmt(_spd_label, "%d kts", ac->speed);
    lv_label_set_text_fmt(_hdg_label, "%03d", ac->heading);

    // Row 2: V/S / SQK / STATUS
    lv_label_set_text_fmt(_vrate_label, "%+d", ac->vert_rate);
    lv_label_set_text_fmt(_squawk_label, "%04d", ac->squawk);

    const char *status;
    if (ac->on_ground) status = "Ground";
    else if (ac->vert_rate > 300) status = "Climb";
    else if (ac->vert_rate < -300) status = "Descend";
    else status = "Cruise";
    lv_label_set_text(_status_label, status);

    // Row 3: DIST / BRG / SIGNAL
    float dist = MapProjection::distance_nm(g_config.home_lat, g_config.home_lon, ac->lat, ac->lon);
    lv_label_set_text_fmt(_dist_label, "%.1fnm", dist);

    float dlon = (ac->lon - g_config.home_lon) * M_PI / 180.0f;
    float y = sinf(dlon) * cosf(ac->lat * M_PI / 180.0f);
    float x = cosf(g_config.home_lat * M_PI / 180.0f) * sinf(ac->lat * M_PI / 180.0f) -
              sinf(g_config.home_lat * M_PI / 180.0f) * cosf(ac->lat * M_PI / 180.0f) * cosf(dlon);
    int bearing = (int)(atan2f(y, x) * 180.0f / M_PI + 360.0f) % 360;
    lv_label_set_text_fmt(_bearing_label, "%03d", bearing);

    uint32_t age_ms = millis() - ac->last_seen;
    if (age_ms < 1000) {
        lv_label_set_text(_signal_label, "live");
    } else {
        lv_label_set_text_fmt(_signal_label, "%lus", (unsigned long)(age_ms / 1000));
    }

    // Slide in
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

    lv_timer_resume(_update_timer);

    // Fetch enrichment
    lv_obj_clear_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);
    enrichment_fetch(ac->icao_hex, ac->registration, ac->callsign, on_enrichment_ready);
}

void detail_card_hide() {
    if (!_visible) return;
    _visible = false;

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
