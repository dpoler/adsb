#include <Arduino.h>
#include "radar_view.h"
#include "views.h"
#include "detail_card.h"
#include "../ui/filters.h"

#include "../pins_s3.h"
#include "../data/storage.h"
#include "../ui/geo.h"
#include "../ui/range.h"

static AircraftList *_list = nullptr;
static lv_obj_t *_radar_obj = nullptr;
static lv_obj_t *_range_label = nullptr;

static float _sweep_angle = 0.0f;
static uint32_t _last_sweep_ms = 0;

#define RADAR_W LCD_H_RES
#define RADAR_H (LCD_V_RES - 24)
#define RADAR_CX (RADAR_W / 2)
#define RADAR_CY (RADAR_H / 2)
#define RADAR_R (RADAR_H / 2 - 10)  // ~138px

#define SWEEP_PERIOD_MS 30000

#define COLOR_SWEEP lv_color_hex(0x00ff44)
#define COLOR_RING lv_color_hex(0x0a2a0a)
#define COLOR_TEXT lv_color_hex(0x00cc33)
#define COLOR_BG lv_color_hex(0x000800)

#define SWEEP_BRIGHT_DEG 60.0f
#define SWEEP_FADE_DEG   180.0f
#define BLIP_NORMAL_OPA  LV_OPA_90
#define LABEL_VISIBLE_DEG 240.0f

static MapProjection _proj;

// Filter buttons
static lv_obj_t *_filter_btns[NUM_FILTERS] = {};
static lv_obj_t *_filter_lbls[NUM_FILTERS] = {};
static bool _filter_just_clicked = false;

// Filter bar height
#define FILTER_BAR_H 32

static bool to_radar_screen(float lat, float lon, int &sx, int &sy) {
    float dx_nm = (lon - _proj.center_lon) * 60.0f * cosf(_proj.center_lat * M_PI / 180.0f);
    float dy_nm = (lat - _proj.center_lat) * 60.0f;
    float dist_nm = sqrtf(dx_nm * dx_nm + dy_nm * dy_nm);
    if (dist_nm > _proj.radius_nm) return false;
    float scale = (float)RADAR_R / _proj.radius_nm;
    sx = RADAR_CX + (int)(dx_nm * scale);
    sy = RADAR_CY - (int)(dy_nm * scale);
    return true;
}

static float blip_angle(int sx, int sy) {
    float dx = (float)(sx - RADAR_CX);
    float dy = (float)(RADAR_CY - sy);
    float angle = atan2f(dx, dy) * 180.0f / M_PI;
    if (angle < 0) angle += 360.0f;
    return angle;
}

static float angle_behind_sweep(float blip_deg) {
    float diff = _sweep_angle - blip_deg;
    if (diff < 0) diff += 360.0f;
    return diff;
}

static void draw_rings(lv_layer_t *layer) {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = COLOR_RING;
    arc.width = 1;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.center.x = RADAR_CX;
    arc.center.y = RADAR_CY;

    float range = range_get_nm();
    float ring_nm;
    if (range >= 100) ring_nm = 25.0f;
    else if (range >= 50) ring_nm = 10.0f;
    else if (range >= 20) ring_nm = 5.0f;
    else ring_nm = 1.0f;

    float scale = (float)RADAR_R / range;
    for (float r = ring_nm; r <= range; r += ring_nm) {
        arc.radius = (int)(r * scale);
        arc.center.x = RADAR_CX;
        arc.center.y = RADAR_CY;
        lv_draw_arc(layer, &arc);
    }

    // Cardinal direction labels
    const char *dirs[] = {"N", "E", "S", "W"};
    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};
    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = COLOR_RING;
    lbl.font = &lv_font_montserrat_14;
    for (int i = 0; i < 4; i++) {
        int lx = RADAR_CX + dx[i] * (RADAR_R + 2) - 5;
        int ly = RADAR_CY + dy[i] * (RADAR_R + 2) - 7;
        lv_area_t area = {(lv_coord_t)lx, (lv_coord_t)ly,
                          (lv_coord_t)(lx + 20), (lv_coord_t)(ly + 16)};
        lbl.text = dirs[i];
        lv_draw_label(layer, &lbl, &area);
    }

    // Cross lines
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_RING;
    line.width = 1;
    line.opa = LV_OPA_30;
    line.p1 = {RADAR_CX, RADAR_CY - RADAR_R};
    line.p2 = {RADAR_CX, RADAR_CY + RADAR_R};
    lv_draw_line(layer, &line);
    line.p1 = {RADAR_CX - RADAR_R, RADAR_CY};
    line.p2 = {RADAR_CX + RADAR_R, RADAR_CY};
    lv_draw_line(layer, &line);
}

static void draw_sweep(lv_layer_t *layer) {
    float rad = _sweep_angle * M_PI / 180.0f;
    int ex = RADAR_CX + (int)(RADAR_R * sinf(rad));
    int ey = RADAR_CY - (int)(RADAR_R * cosf(rad));

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_SWEEP;
    line.width = 2;
    line.opa = LV_OPA_60;
    line.p1 = {RADAR_CX, RADAR_CY};
    line.p2 = {(lv_value_precise_t)ex, (lv_value_precise_t)ey};
    lv_draw_line(layer, &line);

    // Fading trail
    for (int i = 1; i <= 6; i++) {
        float trail_rad = (_sweep_angle - i * 3.0f) * M_PI / 180.0f;
        int tx = RADAR_CX + (int)(RADAR_R * sinf(trail_rad));
        int ty = RADAR_CY - (int)(RADAR_R * cosf(trail_rad));
        line.opa = LV_OPA_40 - i * 6;
        line.width = 1;
        line.p1 = {RADAR_CX, RADAR_CY};
        line.p2 = {(lv_value_precise_t)tx, (lv_value_precise_t)ty};
        lv_draw_line(layer, &line);
    }
}

static void draw_blips(lv_layer_t *layer) {
    if (!_list->lock(pdMS_TO_TICKS(5))) return;

    uint32_t now = millis();

    for (int i = 0; i < _list->count; i++) {
        Aircraft &ac = _list->aircraft[i];
        uint8_t ghost_opa = compute_aircraft_opacity(ac.stale_since, now);
        if (ghost_opa == 0) continue;
        if (!aircraft_passes_filter(ac)) continue;

        int sx, sy;
        if (!to_radar_screen(ac.lat, ac.lon, sx, sy)) continue;

        float behind = angle_behind_sweep(blip_angle(sx, sy));
        uint8_t sweep_opa;
        if (behind < SWEEP_BRIGHT_DEG) {
            sweep_opa = LV_OPA_COVER;
        } else if (behind < SWEEP_FADE_DEG) {
            float t = (behind - SWEEP_BRIGHT_DEG) / (SWEEP_FADE_DEG - SWEEP_BRIGHT_DEG);
            sweep_opa = LV_OPA_COVER - (uint8_t)(t * (LV_OPA_COVER - BLIP_NORMAL_OPA));
        } else {
            sweep_opa = BLIP_NORMAL_OPA;
        }

        uint8_t opa = (uint8_t)((sweep_opa * ghost_opa) / 255);

        // Category color
        lv_color_t color;
        if (ac.is_emergency)  color = COLOR_EMERGENCY;
        else if (ac.is_military)   color = COLOR_MILITARY;
        else if ((ac.category[0] == 'A' && ac.category[1] == '7') ||
                 (ac.type_code[0] && is_heli_type(ac.type_code)))
            color = COLOR_HELI_CAT;
        else if (is_airline_callsign(ac.callsign) ||
                 (ac.category[0] == 'A' && ac.category[1] >= '3'))
            color = COLOR_COMMERCIAL;
        else
            color = COLOR_GA_PRIVATE;

        // Trail segments
        if (ac.trail_count > 1 && g_config.trails_enabled) {
            int max_pts = g_config.trail_max_points;
            int start = (ac.trail_count > max_pts) ? ac.trail_count - max_pts : 0;
            lv_draw_rect_dsc_t tdot;
            lv_draw_rect_dsc_init(&tdot);
            tdot.radius = 1;
            for (int t = start; t < ac.trail_count; t++) {
                int tx, ty;
                if (to_radar_screen(ac.trail[t].lat, ac.trail[t].lon, tx, ty)) {
                    tdot.bg_color = altitude_color(ac.trail[t].alt);
                    uint8_t t_opa = (uint8_t)(LV_OPA_20 + ((t - start) * LV_OPA_40 / (ac.trail_count - start)));
                    tdot.bg_opa = (uint8_t)((t_opa * opa) / 255);
                    lv_area_t ta = {(lv_coord_t)(tx - 1), (lv_coord_t)(ty - 1),
                                    (lv_coord_t)(tx + 1), (lv_coord_t)(ty + 1)};
                    lv_draw_rect(layer, &tdot, &ta);
                }
            }
        }

        // Blip dot — 3px radius (smaller than P4's 4px)
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = color;
        dot.bg_opa = opa;
        dot.radius = 3;
        lv_area_t area = {(lv_coord_t)(sx - 3), (lv_coord_t)(sy - 3),
                          (lv_coord_t)(sx + 3), (lv_coord_t)(sy + 3)};
        lv_draw_rect(layer, &dot, &area);

        // Labels — callsign only (1 line for S3 smaller screen)
        if (behind < LABEL_VISIBLE_DEG) {
            uint8_t lbl_opa = opa > LV_OPA_50 ? LV_OPA_80 : opa;
            const char *cs = ac.callsign[0] ? ac.callsign : ac.icao_hex;

            lv_draw_label_dsc_t lbl;
            lv_draw_label_dsc_init(&lbl);
            lbl.color = color;
            lbl.font = &lv_font_montserrat_14;
            lbl.opa = lbl_opa;
            lbl.text = cs;
            lv_area_t la = {(lv_coord_t)(sx + 6), (lv_coord_t)(sy - 7),
                            (lv_coord_t)(sx + 120), (lv_coord_t)(sy + 7)};
            lv_draw_label(layer, &lbl, &la);
        }
    }

    _list->unlock();
}

static void draw_filter_label(lv_layer_t *layer) {
    char buf[96];
    lv_color_t color;
    if (filter_label_text(buf, sizeof(buf), &color) == 0) return;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color;
    lbl.font = &lv_font_montserrat_14;
    lbl.opa = LV_OPA_COVER;
    lbl.text = buf;
    lv_area_t la = {(lv_coord_t)8, (lv_coord_t)4,
                    (lv_coord_t)250, (lv_coord_t)20};
    lv_draw_label(layer, &lbl, &la);
}

static void radar_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_rings(layer);
    draw_sweep(layer);
    draw_blips(layer);
    draw_filter_label(layer);
}

static void update_filter_visuals() {
    unsigned active = filter_get_active();
    for (int i = 0; i < NUM_FILTERS; i++) {
        if (active & (1u << i)) {
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
    if (_radar_obj) lv_obj_invalidate(_radar_obj);
}

static void radar_filter_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _filter_just_clicked = true;
    filter_toggle(idx);
    update_filter_visuals();
}

void radar_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _proj.center_lat = g_config.home_lat;
    _proj.center_lon = g_config.home_lon;
    _proj.radius_nm = range_get_nm();

    // Radar draw area (above filter bar)
    int radar_draw_h = RADAR_H - FILTER_BAR_H;
    _radar_obj = lv_obj_create(parent);
    lv_obj_set_size(_radar_obj, RADAR_W, radar_draw_h);
    lv_obj_set_pos(_radar_obj, 0, 0);
    lv_obj_set_style_bg_color(_radar_obj, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(_radar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_radar_obj, 0, 0);
    lv_obj_set_style_radius(_radar_obj, 0, 0);
    lv_obj_clear_flag(_radar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_radar_obj, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_add_event_cb(_radar_obj, radar_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    // Tap to select aircraft (40px hit radius for resistive touch)
    lv_obj_add_event_cb(_radar_obj, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_RADAR) return;

        if (_filter_just_clicked) {
            _filter_just_clicked = false;
            return;
        }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int tx = point.x;
        int ty = point.y - 24; // offset for status bar

        if (detail_card_is_visible()) {
            detail_card_hide();
            return;
        }

        if (!_list->lock(pdMS_TO_TICKS(10))) return;
        for (int i = 0; i < _list->count; i++) {
            int sx, sy;
            if (to_radar_screen(_list->aircraft[i].lat, _list->aircraft[i].lon, sx, sy)) {
                int dx = tx - sx;
                int dy = ty - sy;
                if (dx * dx + dy * dy < 1600) { // 40px radius
                    Aircraft ac_copy = _list->aircraft[i];
                    _list->unlock();
                    detail_card_show(&ac_copy);
                    return;
                }
            }
        }
        _list->unlock();
    }, LV_EVENT_PRESSED, nullptr);

    // Horizontal filter bar at bottom (same as map view)
    {
        int btn_w = 52, btn_h = 28, btn_gap = 6;
        int total_w = NUM_FILTERS * btn_w + (NUM_FILTERS - 1) * btn_gap;
        int btn_x0 = (RADAR_W - total_w) / 2;
        int btn_y = radar_draw_h + 2;
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
            lv_obj_add_event_cb(btn, radar_filter_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, filter_defs[i].label);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_center(lbl);

            _filter_btns[i] = btn;
            _filter_lbls[i] = lbl;
        }
    }

    // Range label — bottom-right, tappable
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_range_label, COLOR_TEXT, 0);
    lv_obj_set_pos(_range_label, RADAR_W - 60, RADAR_H - FILTER_BAR_H - 22);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
        _proj.radius_nm = range_get_nm();
        lv_obj_invalidate(_radar_obj);
    }, LV_EVENT_CLICKED, nullptr);

    // Sweep animation timer
    _last_sweep_ms = millis();
    static unsigned _last_synced_filter = ~0u; // impossible bitmask value, forces sync on first tick
    static float _last_range = -1;
    lv_timer_create([](lv_timer_t *t) {
        uint32_t now = millis();
        uint32_t dt = now - _last_sweep_ms;
        _last_sweep_ms = now;
        _sweep_angle += (360.0f * dt) / SWEEP_PERIOD_MS;
        if (_sweep_angle >= 360.0f) _sweep_angle -= 360.0f;

        float r = range_get_nm();
        if (r != _last_range) {
            _last_range = r;
            _proj.radius_nm = r;
            lv_label_set_text(_range_label, range_label());
        }

        unsigned af = filter_get_active();
        if (af != _last_synced_filter) {
            _last_synced_filter = af;
            update_filter_visuals();
        }

        if (touch_active) return;

        if (views_get_active_index() == VIEW_RADAR) {
            lv_obj_invalidate(_radar_obj);
        }
    }, 100, nullptr);
}

void radar_view_update() {
    if (_radar_obj) lv_obj_invalidate(_radar_obj);
}

void radar_view_set_home(float lat, float lon) {
    _proj.center_lat = lat;
    _proj.center_lon = lon;
    if (_radar_obj) lv_obj_invalidate(_radar_obj);
}
