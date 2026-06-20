#include <Arduino.h>
#include "map_view.h"
#include "detail_card.h"
#include "views.h"
#include "../ui/range.h"
#include "../ui/filters.h"

#include "../pins_s3.h"
#include "../data/storage.h"
#include "../data/fetcher.h"
#include "../data/error_log.h"

#if __has_include("static_map_data.h")
#include "static_map_data.h"
#define HAS_STATIC_MAP 1
#else
#define HAS_STATIC_MAP 0
#endif

static AircraftList *_list = nullptr;
static lv_obj_t *_canvas = nullptr;
static lv_obj_t *_range_label = nullptr;
static MapProjection _proj;

static bool _filter_just_clicked = false;
static char _tracked_hex[7] = {};

#define CANVAS_W LCD_H_RES
#define CANVAS_H (LCD_V_RES - 24)
#define BG_COLOR lv_color_hex(0x0a0a1a)

static lv_obj_t *_filter_btns[NUM_FILTERS] = {};
static lv_obj_t *_filter_lbls[NUM_FILTERS] = {};

// Loading overlay
static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_overlay_spinner = nullptr;
static lv_obj_t *_overlay_status = nullptr;
static lv_obj_t *_overlay_net = nullptr;
static lv_obj_t *_overlay_stats = nullptr;
static lv_obj_t *_overlay_error = nullptr;
static bool _overlay_dismissed = false;

static void overlay_dismiss_anim_cb(lv_anim_t *a) {
    if (_overlay) {
        lv_obj_delete(_overlay);
        _overlay = nullptr;
        _overlay_spinner = nullptr;
        _overlay_status = nullptr;
        _overlay_net = nullptr;
        _overlay_stats = nullptr;
        _overlay_error = nullptr;
    }
}

static void overlay_dismiss() {
    if (!_overlay || _overlay_dismissed) return;
    _overlay_dismissed = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _overlay);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    });
    lv_anim_set_completed_cb(&a, overlay_dismiss_anim_cb);
    lv_anim_start(&a);
}

static void overlay_update() {
    if (!_overlay || _overlay_dismissed) return;
    const FetcherStats *fs = fetcher_get_stats();
    NetType net = fetcher_connection_type();
    if (_list->count > 0) { overlay_dismiss(); return; }
    if (net == NET_NONE)
        lv_label_set_text(_overlay_status, "Connecting...");
    else if (fs->fetch_ok == 0 && fs->fetch_fail == 0)
        lv_label_set_text(_overlay_status, "Fetching...");
    else
        lv_label_set_text(_overlay_status, "Waiting...");

    if (net != NET_NONE)
        lv_label_set_text_fmt(_overlay_net, "WiFi  %s", fs->ip_addr);
    else
        lv_label_set_text(_overlay_net, "WiFi...");

    if (fs->fetch_ok > 0 || fs->fetch_fail > 0) {
        lv_label_set_text_fmt(_overlay_stats, "%lu ok / %lu err",
            (unsigned long)fs->fetch_ok, (unsigned long)fs->fetch_fail);
    }

    ErrorSnapshot snap = error_log_snapshot();
    if (snap.count > 0) {
        lv_label_set_text(_overlay_error, snap.entries[snap.count - 1].msg);
        lv_obj_set_style_text_color(_overlay_error, lv_color_hex(0xff6666), 0);
    }
}

static void overlay_create(lv_obj_t *parent) {
    int ow = 240, oh = 160;
    _overlay = lv_obj_create(parent);
    lv_obj_set_size(_overlay, ow, oh);
    lv_obj_set_pos(_overlay, (CANVAS_W - ow) / 2, (CANVAS_H - oh) / 2);
    lv_obj_set_style_bg_color(_overlay, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(_overlay, lv_color_hex(0x4488ff), 0);
    lv_obj_set_style_border_width(_overlay, 1, 0);
    lv_obj_set_style_border_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_radius(_overlay, 10, 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLL_CHAIN);

    _overlay_spinner = lv_spinner_create(_overlay);
    lv_spinner_set_anim_params(_overlay_spinner, 1000, 270);
    lv_obj_clear_flag(_overlay_spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(_overlay_spinner, 32, 32);
    lv_obj_set_pos(_overlay_spinner, (ow - 32) / 2, 10);
    lv_obj_set_style_arc_color(_overlay_spinner, lv_color_hex(0x4488ff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_overlay_spinner, lv_color_hex(0x1a2a3a), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_overlay_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(_overlay_spinner, 3, LV_PART_MAIN);

    _overlay_status = lv_label_create(_overlay);
    lv_label_set_text(_overlay_status, "Connecting...");
    lv_obj_set_style_text_font(_overlay_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_overlay_status, lv_color_hex(0xccccdd), 0);
    lv_obj_set_style_text_align(_overlay_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_overlay_status, ow - 16);
    lv_obj_set_pos(_overlay_status, 8, 48);

    _overlay_net = lv_label_create(_overlay);
    lv_label_set_text(_overlay_net, "WiFi...");
    lv_obj_set_style_text_font(_overlay_net, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_overlay_net, lv_color_hex(0x4488ff), 0);
    lv_obj_set_style_text_align(_overlay_net, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_overlay_net, ow - 16);
    lv_obj_set_pos(_overlay_net, 8, 72);

    _overlay_stats = lv_label_create(_overlay);
    lv_label_set_text(_overlay_stats, "");
    lv_obj_set_style_text_font(_overlay_stats, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_overlay_stats, lv_color_hex(0x888899), 0);
    lv_obj_set_style_text_align(_overlay_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_overlay_stats, ow - 16);
    lv_obj_set_pos(_overlay_stats, 8, 94);

    _overlay_error = lv_label_create(_overlay);
    lv_label_set_text(_overlay_error, "");
    lv_obj_set_style_text_font(_overlay_error, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_overlay_error, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(_overlay_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_overlay_error, ow - 16);
    lv_obj_set_pos(_overlay_error, 8, 118);
}

static void update_filter_visuals() {
    int active = filter_get_active();
    for (int i = 0; i < NUM_FILTERS; i++) {
        if (active == i) {
            lv_obj_set_style_bg_color(_filter_btns[i], filter_defs[i].color, 0);
            lv_obj_set_style_bg_opa(_filter_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(_filter_btns[i], lv_color_hex(0xffffff), 0);
            lv_obj_set_style_border_width(_filter_btns[i], 2, 0);
            lv_obj_set_style_border_opa(_filter_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(_filter_lbls[i], lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(_filter_btns[i], lv_color_hex(0x0a0a1a), 0);
            lv_obj_set_style_bg_opa(_filter_btns[i], LV_OPA_70, 0);
            lv_obj_set_style_border_color(_filter_btns[i], filter_defs[i].color, 0);
            lv_obj_set_style_border_width(_filter_btns[i], 1, 0);
            lv_obj_set_style_border_opa(_filter_btns[i], LV_OPA_40, 0);
            lv_obj_set_style_text_color(_filter_lbls[i], lv_color_hex(0x666666), 0);
        }
    }
    if (_canvas) lv_obj_invalidate(_canvas);
}

static void filter_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _filter_just_clicked = true;
    filter_toggle(idx);
    update_filter_visuals();
}

static void draw_grid(lv_layer_t *layer) {
    float radius_nm = range_get_nm();
    float cos_lat = cosf(_proj.center_lat * M_PI / 180.0f);
    float grid_deg = radius_nm >= 30 ? 1.0f : (radius_nm >= 10 ? 0.5f : 0.1f);
    float half_h_nm = radius_nm;
    float half_w_nm = radius_nm * (float)CANVAS_W / (float)CANVAS_H;
    float north = _proj.center_lat + half_h_nm / 60.0f;
    float south = _proj.center_lat - half_h_nm / 60.0f;
    float east = _proj.center_lon + half_w_nm / (60.0f * cos_lat);
    float west = _proj.center_lon - half_w_nm / (60.0f * cos_lat);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x0d1a2a);
    line_dsc.width = 1;
    line_dsc.opa = LV_OPA_COVER;

    float lon_start = floorf(west / grid_deg) * grid_deg;
    for (float lon = lon_start; lon <= east; lon += grid_deg) {
        int x1, y1, x2, y2;
        _proj.to_screen(north, lon, x1, y1);
        _proj.to_screen(south, lon, x2, y2);
        line_dsc.p1 = {(lv_value_precise_t)x1, (lv_value_precise_t)y1};
        line_dsc.p2 = {(lv_value_precise_t)x2, (lv_value_precise_t)y2};
        lv_draw_line(layer, &line_dsc);
    }
    float lat_start = floorf(south / grid_deg) * grid_deg;
    for (float lat = lat_start; lat <= north; lat += grid_deg) {
        int x1, y1, x2, y2;
        _proj.to_screen(lat, west, x1, y1);
        _proj.to_screen(lat, east, x2, y2);
        line_dsc.p1 = {(lv_value_precise_t)x1, (lv_value_precise_t)y1};
        line_dsc.p2 = {(lv_value_precise_t)x2, (lv_value_precise_t)y2};
        lv_draw_line(layer, &line_dsc);
    }
}

static void draw_range_rings(lv_layer_t *layer) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = lv_color_hex(0x1a2a3a);
    arc_dsc.width = 1;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    float radius_nm = range_get_nm();
    float scale = (float)CANVAS_H / (radius_nm * 2.0f);
    float ring_interval = radius_nm <= 10 ? 2.0f : (radius_nm <= 25 ? 5.0f : 10.0f);
    for (float r = ring_interval; r <= radius_nm; r += ring_interval) {
        int pixel_r = (int)(r * scale);
        arc_dsc.center.x = CANVAS_W / 2 + _proj.offset_x;
        arc_dsc.center.y = CANVAS_H / 2 + _proj.offset_y;
        arc_dsc.radius = pixel_r;
        lv_draw_arc(layer, &arc_dsc);
    }
}

static void draw_home_marker(lv_layer_t *layer) {
    int hx, hy;
    if (!_proj.to_screen(g_config.home_lat, g_config.home_lon, hx, hy)) return;
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x4488ff);
    line_dsc.width = 1;
    line_dsc.p1 = {(lv_value_precise_t)(hx - 6), (lv_value_precise_t)hy};
    line_dsc.p2 = {(lv_value_precise_t)(hx + 6), (lv_value_precise_t)hy};
    lv_draw_line(layer, &line_dsc);
    line_dsc.p1 = {(lv_value_precise_t)hx, (lv_value_precise_t)(hy - 6)};
    line_dsc.p2 = {(lv_value_precise_t)hx, (lv_value_precise_t)(hy + 6)};
    lv_draw_line(layer, &line_dsc);
}

static inline void rotate_pt(float lx, float ly, float sin_h, float cos_h,
                              int cx, int cy, lv_point_precise_t &out) {
    out.x = (lv_value_precise_t)(cx + lx * cos_h - ly * sin_h);
    out.y = (lv_value_precise_t)(cy + lx * sin_h + ly * cos_h);
}

static void draw_tri(lv_layer_t *layer, int cx, int cy, float sin_h, float cos_h,
                     float x0, float y0, float x1, float y1, float x2, float y2,
                     lv_color_t color, uint8_t opa) {
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = opa;
    rotate_pt(x0, y0, sin_h, cos_h, cx, cy, tri.p[0]);
    rotate_pt(x1, y1, sin_h, cos_h, cx, cy, tri.p[1]);
    rotate_pt(x2, y2, sin_h, cos_h, cx, cy, tri.p[2]);
    lv_draw_triangle(layer, &tri);
}

// ~60% of P4 airliner (22px long from 36px)
static void draw_icon_airliner(lv_layer_t *layer, int cx, int cy,
                                float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -11, 1, 0, -1, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -11, 1, 0, 0, 11, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -11, -1, 0, 0, 11, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -2, 9, 1, 0, 3, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -2, -9, 1, 0, 3, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, 8, 4, 10, 0, 11, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, 8, -4, 10, 0, 11, color, opa);
}

static void draw_icon_jet(lv_layer_t *layer, int cx, int cy,
                          float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -5, 1, 2, -1, 2, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -5, 1, 2, 0, 5, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -5, -1, 2, 0, 5, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -1, 6, 2, 0, 2, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -1, -6, 2, 0, 2, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, 3, 3, 5, 0, 5, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, 3, -3, 5, 0, 5, color, opa);
}

static void draw_icon_ga(lv_layer_t *layer, int cx, int cy,
                         float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -3, 1, 0, -1, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 1, 0, -1, 0, 0, 3, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -1, 5, -1, 0, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -1, -5, -1, 0, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, 2, 2, 3, -2, 3, color, opa);
}

static void draw_icon_heli(lv_layer_t *layer, int cx, int cy,
                           float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    draw_tri(layer, cx, cy, sin_h, cos_h, 0, -3, 2, 1, -2, 1, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h, 2, 1, -2, 1, 0, 4, color, opa);
    lv_draw_line_dsc_t rotor;
    lv_draw_line_dsc_init(&rotor);
    rotor.color = color;
    rotor.width = 1;
    rotor.opa = (uint8_t)(opa * 3 / 4);
    lv_point_precise_t r0, r1, r2, r3;
    rotate_pt(-5, -1, sin_h, cos_h, cx, cy, r0);
    rotate_pt( 5, -1, sin_h, cos_h, cx, cy, r1);
    rotate_pt( 0, -5, sin_h, cos_h, cx, cy, r2);
    rotate_pt( 0,  3, sin_h, cos_h, cx, cy, r3);
    rotor.p1 = {r0.x, r0.y}; rotor.p2 = {r1.x, r1.y};
    lv_draw_line(layer, &rotor);
    rotor.p1 = {r2.x, r2.y}; rotor.p2 = {r3.x, r3.y};
    lv_draw_line(layer, &rotor);
}

enum IconType { ICON_AIRLINER, ICON_JET, ICON_GA, ICON_HELI };

static IconType classify_icon(const Aircraft &ac) {
    if (ac.category[0] == 'A' && ac.category[1] == '7') return ICON_HELI;
    if (ac.type_code[0] && is_heli_type(ac.type_code)) return ICON_HELI;
    if (ac.is_military) return ICON_JET;
    if (is_airline_callsign(ac.callsign)) return ICON_AIRLINER;
    if (ac.category[0] == 'A' && ac.category[1] >= '3') return ICON_AIRLINER;
    return ICON_GA;
}

static void draw_aircraft(lv_layer_t *layer) {
    if (!_list->lock(pdMS_TO_TICKS(5))) return;
    uint32_t now = millis();

    for (int i = 0; i < _list->count; i++) {
        Aircraft &ac = _list->aircraft[i];
        uint8_t ac_opa = compute_aircraft_opacity(ac.stale_since, now);
        if (ac_opa == 0) continue;
        if (!aircraft_passes_filter(ac)) continue;

        int sx, sy;
        if (!_proj.to_screen(ac.lat, ac.lon, sx, sy)) continue;

        IconType icon = classify_icon(ac);
        lv_color_t color;
        if (ac.is_emergency)       color = COLOR_EMERGENCY;
        else if (ac.is_military)   color = COLOR_MILITARY;
        else if (icon == ICON_HELI) color = COLOR_HELI_CAT;
        else if (icon == ICON_GA)  color = COLOR_GA_PRIVATE;
        else                       color = COLOR_COMMERCIAL;

        // Trails
        if (ac.trail_count > 1 && g_config.trails_enabled) {
            int max_pts = g_config.trail_max_points;
            int start = (ac.trail_count > max_pts) ? ac.trail_count - max_pts : 0;
            if (start < 1) start = 1;
            lv_draw_line_dsc_t trail_dsc;
            lv_draw_line_dsc_init(&trail_dsc);
            trail_dsc.width = 1;
            for (int t = start; t < ac.trail_count; t++) {
                int tx1, ty1, tx2, ty2;
                if (_proj.to_screen(ac.trail[t - 1].lat, ac.trail[t - 1].lon, tx1, ty1) &&
                    _proj.to_screen(ac.trail[t].lat, ac.trail[t].lon, tx2, ty2)) {
                    trail_dsc.color = altitude_color(ac.trail[t].alt);
                    trail_dsc.opa = LV_OPA_50 + (t * LV_OPA_50 / ac.trail_count);
                    trail_dsc.p1 = {(lv_value_precise_t)tx1, (lv_value_precise_t)ty1};
                    trail_dsc.p2 = {(lv_value_precise_t)tx2, (lv_value_precise_t)ty2};
                    lv_draw_line(layer, &trail_dsc);
                }
            }
        }

        float heading_rad = ac.heading * M_PI / 180.0f;
        float sin_h = sinf(heading_rad);
        float cos_h = cosf(heading_rad);

        switch (icon) {
            case ICON_AIRLINER: draw_icon_airliner(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
            case ICON_JET:      draw_icon_jet(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
            case ICON_GA:       draw_icon_ga(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
            case ICON_HELI:     draw_icon_heli(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
        }

        // Tracking circle
        if (_tracked_hex[0] && strcmp(ac.icao_hex, _tracked_hex) == 0) {
            lv_draw_arc_dsc_t ring;
            lv_draw_arc_dsc_init(&ring);
            ring.color = lv_color_hex(0xff2222);
            ring.width = 2;
            ring.start_angle = 0;
            ring.end_angle = 360;
            ring.center.x = sx;
            ring.center.y = sy;
            ring.radius = 16;
            ring.opa = LV_OPA_COVER;
            lv_draw_arc(layer, &ring);
        }

        // Callsign label
        const char *label_text = ac.callsign[0] ? ac.callsign : ac.icao_hex;
        lv_draw_label_dsc_t lbl_dsc;
        lv_draw_label_dsc_init(&lbl_dsc);
        lbl_dsc.color = color;
        lbl_dsc.font = &lv_font_montserrat_14;
        lbl_dsc.opa = (uint8_t)((ac_opa * LV_OPA_80) / 255);
        lv_area_t lbl_area = {(lv_coord_t)(sx + 10), (lv_coord_t)(sy - 7),
                               (lv_coord_t)(sx + 100), (lv_coord_t)(sy + 7)};
        lbl_dsc.text = label_text;
        lv_draw_label(layer, &lbl_dsc, &lbl_area);
    }
    _list->unlock();
}

static void draw_filter_label(lv_layer_t *layer) {
    int af = filter_get_active();
    if (af == FILT_NONE) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "FILTER: %s", filter_defs[af].full_name);
    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = filter_defs[af].color;
    lbl.font = &lv_font_montserrat_14;
    lbl.opa = LV_OPA_COVER;
    lbl.text = buf;
    lv_area_t la = {4, 4, 250, 18};
    lv_draw_label(layer, &lbl, &la);
}

static void canvas_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_grid(layer);
    draw_range_rings(layer);
    draw_home_marker(layer);
    draw_aircraft(layer);
    draw_filter_label(layer);
}

void map_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _proj.center_lat = g_config.home_lat;
    _proj.center_lon = g_config.home_lon;
    _proj.radius_nm = range_get_nm();
    _proj.screen_w = CANVAS_W;
    _proj.screen_h = CANVAS_H;
    _proj.offset_x = 0;
    _proj.offset_y = 0;

    _canvas = lv_obj_create(parent);
    lv_obj_set_size(_canvas, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(_canvas, 0, 0);
    lv_obj_set_style_bg_color(_canvas, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_canvas, 0, 0);
    lv_obj_set_style_radius(_canvas, 0, 0);
    lv_obj_set_style_pad_all(_canvas, 0, 0);
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_add_event_cb(_canvas, canvas_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    // Tap: aircraft hit-test
    lv_obj_add_event_cb(_canvas, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_MAP) return;
        if (_filter_just_clicked) { _filter_just_clicked = false; return; }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int tx = point.x;
        int ty = point.y - 24;

        if (detail_card_is_visible()) { detail_card_hide(); return; }

        if (!_list->lock(pdMS_TO_TICKS(10))) return;
        for (int i = 0; i < _list->count; i++) {
            int sx, sy;
            if (_proj.to_screen(_list->aircraft[i].lat, _list->aircraft[i].lon, sx, sy)) {
                int dx = tx - sx;
                int dy = ty - sy;
                if (dx * dx + dy * dy < 1600) {  // 40px hit radius
                    Aircraft ac_copy = _list->aircraft[i];
                    strlcpy(_tracked_hex, ac_copy.icao_hex, sizeof(_tracked_hex));
                    _list->unlock();
                    detail_card_show(&ac_copy);
                    return;
                }
            }
        }
        _list->unlock();
        _tracked_hex[0] = '\0';
    }, LV_EVENT_PRESSED, nullptr);

    // Filter buttons — horizontal row at bottom
    int btn_w = 52, btn_h = 28, btn_gap = 4;
    int total_w = NUM_FILTERS * btn_w + (NUM_FILTERS - 1) * btn_gap;
    int btn_x0 = (CANVAS_W - total_w) / 2;
    int btn_y = CANVAS_H - btn_h - 4;
    for (int i = 0; i < NUM_FILTERS; i++) {
        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, btn_x0 + i * (btn_w + btn_gap), btn_y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_border_color(btn, filter_defs[i].color, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(btn, filter_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, filter_defs[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_center(lbl);

        _filter_btns[i] = btn;
        _filter_lbls[i] = lbl;
    }

    // Range label
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_range_label, lv_color_hex(0x4488ff), 0);
    lv_obj_set_pos(_range_label, CANVAS_W - 60, CANVAS_H - btn_h - 28);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
        _proj.radius_nm = range_get_nm();
        lv_obj_invalidate(_canvas);
    }, LV_EVENT_CLICKED, nullptr);

    overlay_create(parent);

    static int _last_synced_filter = FILT_NONE;
    static float _last_range = -1;
    lv_timer_create([](lv_timer_t *t) {
        int af = filter_get_active();
        if (af != _last_synced_filter) {
            _last_synced_filter = af;
            update_filter_visuals();
        }
        overlay_update();
        if (touch_active) return;
        if (views_get_active_index() == VIEW_MAP) {
            float r = range_get_nm();
            if (r != _last_range) {
                _last_range = r;
                _proj.radius_nm = r;
                lv_label_set_text(_range_label, range_label());
            }
            lv_obj_invalidate(_canvas);
        }
    }, 1000, nullptr);
}

void map_view_update() { if (_canvas) lv_obj_invalidate(_canvas); }
void map_view_center_on(float lat, float lon) {
    _proj.center_lat = lat; _proj.center_lon = lon;
    if (_canvas) lv_obj_invalidate(_canvas);
}
void map_view_track(const char *icao_hex) {
    if (icao_hex && icao_hex[0]) strlcpy(_tracked_hex, icao_hex, sizeof(_tracked_hex));
    else _tracked_hex[0] = '\0';
}
