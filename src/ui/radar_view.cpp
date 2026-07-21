#include <Arduino.h>
#include "radar_view.h"
#include "views.h"
#include "status_bar.h"
#include "detail_card.h"
#include "filters.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include "../data/locations.h"
#include "geo.h"
#include "range.h"

static AircraftList *_list = nullptr;      // currently effective list
static AircraftList *_home_list = nullptr; // the list passed in at init
static int _last_active_loc = -2;          // sentinel — -1 is a valid "Home"
static lv_obj_t *_radar_obj = nullptr;

static float _sweep_angle = 0.0f; // current sweep angle in degrees
static uint32_t _last_sweep_ms = 0;
// Per-view "clear trails" cutoff -- Map and Radar share the same underlying
// Aircraft/TrailPoint data, so clearing trails must not mutate that shared
// data (it would clear both views at once). Instead each view independently
// stops drawing any trail point older than its own cutoff.
static uint32_t _trails_cleared_at = 0;

#define RADAR_W LCD_H_RES
#define RADAR_H (LCD_V_RES - STATUS_BAR_HEIGHT)
#define RADAR_CX (RADAR_W / 2)
// Extra clearance above the compass rose so its "N" label / outer ring
// don't touch the status bar directly above the canvas -- without this the
// radius is scaled to reach exactly y=0 (zero gap). Shrinks the effective
// drawing height and recenters down instead of touching the top edge.
#define RADAR_TOP_MARGIN 22
#define RADAR_CY (RADAR_H / 2 + RADAR_TOP_MARGIN / 2)
#define RADAR_R ((RADAR_H - RADAR_TOP_MARGIN) / 2 - 10)  // max radius in pixels

#define SWEEP_PERIOD_MS 30000  // one full rotation = 30 seconds

#define COLOR_SWEEP lv_color_hex(0x00ff44)
#define COLOR_RING lv_color_hex(0x0a2a0a)
#define COLOR_TEXT lv_color_hex(0x00cc33)
#define COLOR_BG lv_color_hex(0x000800)

#define SWEEP_BRIGHT_DEG 60.0f        // full brightness boost zone
#define SWEEP_FADE_DEG   180.0f       // fade boost back to normal
#define BLIP_NORMAL_OPA  LV_OPA_90    // normal blip brightness
#define PAINT_DETAIL_DEG 45.0f        // expanded detail zone right after sweep
#define LABEL_VISIBLE_DEG 240.0f      // callsign label visibility zone

static MapProjection _proj;

// Per-view filter button/label pointers
static lv_obj_t *_filter_btns[NUM_FILTERS] = {};
static lv_obj_t *_filter_lbls[NUM_FILTERS] = {};
static bool _filter_just_clicked = false;

// Convert lat/lon to radar-relative screen coords
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

// Get bearing angle (0=N, 90=E) from radar center to a screen point
static float blip_angle(int sx, int sy) {
    float dx = (float)(sx - RADAR_CX);
    float dy = (float)(RADAR_CY - sy); // invert Y (screen Y goes down)
    float angle = atan2f(dx, dy) * 180.0f / M_PI;
    if (angle < 0) angle += 360.0f;
    return angle;
}

// Angular distance from sweep to blip (how far behind the sweep the blip is)
static float angle_behind_sweep(float blip_deg) {
    float diff = _sweep_angle - blip_deg;
    if (diff < 0) diff += 360.0f;
    return diff; // 0 = just swept, 359 = about to be swept
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

    // Sweep line
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_SWEEP;
    line.width = 2;
    line.opa = LV_OPA_60;
    line.p1 = {RADAR_CX, RADAR_CY};
    line.p2 = {(lv_value_precise_t)ex, (lv_value_precise_t)ey};
    lv_draw_line(layer, &line);

    // Fading trail (draw 6 faded lines behind sweep)
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
    if (!_list->lock(pdMS_TO_TICKS(5))) return; // short timeout: skip frame if data locked

    uint32_t now = millis();

    for (int i = 0; i < _list->count; i++) {
        Aircraft &ac = _list->aircraft[i];
        uint8_t ghost_opa = compute_aircraft_opacity(ac.stale_since, now);
        if (ghost_opa == 0) continue;
        if (!aircraft_passes_filter(ac)) continue;
        if (g_config.hide_ground && ac.on_ground) continue;

        int sx, sy;
        if (!to_radar_screen(ac.lat, ac.lon, sx, sy)) continue;

        // Sweep boost: brief brightness flash when sweep passes, otherwise normal
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

        // Combine sweep brightness with ghost fade (stale aircraft fade out)
        uint8_t opa = (uint8_t)((sweep_opa * ghost_opa) / 255);

        // Category color for blip
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

        // Draw trail segments (if enabled) — altitude-colored
        if (ac.trail_count > 1 && g_config.trails_enabled) {
            int max_pts = g_config.trail_max_points;
            int start = (ac.trail_count > max_pts) ? ac.trail_count - max_pts : 0;
            lv_draw_rect_dsc_t tdot;
            lv_draw_rect_dsc_init(&tdot);
            tdot.radius = 1;
            for (int t = start; t < ac.trail_count; t++) {
                if (ac.trail[t].timestamp < _trails_cleared_at) continue;
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

        // Blip dot — larger for better visibility
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = color;
        dot.bg_opa = opa;
        dot.radius = 4;
        lv_area_t area = {(lv_coord_t)(sx - 4), (lv_coord_t)(sy - 4),
                          (lv_coord_t)(sx + 4), (lv_coord_t)(sy + 4)};
        lv_draw_rect(layer, &dot, &area);

        // Labels — two zones: paint detail (0-45deg) and condensed (45-240deg)
        if (behind < LABEL_VISIBLE_DEG) {
            uint8_t lbl_opa = opa > LV_OPA_50 ? LV_OPA_80 : opa;

            if (behind < PAINT_DETAIL_DEG) {
                // === PAINT ZONE: expanded detail ===
                // Line 1: Callsign
                const char *cs = ac.callsign[0] ? ac.callsign : ac.icao_hex;
                char line1[48];
                strlcpy(line1, cs, sizeof(line1));
                lv_draw_label_dsc_t l1;
                lv_draw_label_dsc_init(&l1);
                l1.color = color;
                l1.font = &lv_font_montserrat_14;
                l1.opa = LV_OPA_COVER;
                l1.text = line1;
                lv_area_t a1 = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy - 28),
                                (lv_coord_t)(sx + 280), (lv_coord_t)(sy - 14)};
                lv_draw_label(layer, &l1, &a1);

                // Line 2: Operator or type description
                char line2[48] = {};
                if (ac.owner_op[0]) strlcpy(line2, ac.owner_op, sizeof(line2));
                else if (ac.desc[0]) strlcpy(line2, ac.desc, sizeof(line2));
                else strlcpy(line2, ac.type_code, sizeof(line2));
                lv_draw_label_dsc_t l2;
                lv_draw_label_dsc_init(&l2);
                l2.color = color;
                l2.font = &lv_font_montserrat_14;
                l2.opa = (uint8_t)(LV_OPA_COVER * 3 / 4);
                l2.text = line2;
                lv_area_t a2 = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy - 14),
                                (lv_coord_t)(sx + 280), (lv_coord_t)(sy)};
                lv_draw_label(layer, &l2, &a2);

                // Line 3: Alt + speed + vert rate
                char line3[48];
                char alt_str[12];
                if (ac.on_ground) snprintf(alt_str, sizeof(alt_str), "GND");
                else if (ac.altitude >= 18000) snprintf(alt_str, sizeof(alt_str), "FL%03d", ac.altitude / 100);
                else snprintf(alt_str, sizeof(alt_str), "%d'", ac.altitude);
                const char *vr_arrow = ac.vert_rate > 300 ? "^" : ac.vert_rate < -300 ? "v" : "";
                snprintf(line3, sizeof(line3), "%s %dkt %s", alt_str, ac.speed, vr_arrow);
                lv_draw_label_dsc_t l3;
                lv_draw_label_dsc_init(&l3);
                l3.color = color;
                l3.font = &lv_font_montserrat_14;
                l3.opa = (uint8_t)(LV_OPA_COVER * 2 / 3);
                l3.text = line3;
                lv_area_t a3 = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy),
                                (lv_coord_t)(sx + 280), (lv_coord_t)(sy + 14)};
                lv_draw_label(layer, &l3, &a3);

                // Line 4: Registration + type code
                char line4[24] = {};
                if (ac.registration[0]) {
                    snprintf(line4, sizeof(line4), "%s %s", ac.registration, ac.type_code);
                }
                if (line4[0]) {
                    lv_draw_label_dsc_t l4;
                    lv_draw_label_dsc_init(&l4);
                    l4.color = color;
                    l4.font = &lv_font_montserrat_14;
                    l4.opa = (uint8_t)(LV_OPA_COVER / 2);
                    l4.text = line4;
                    lv_area_t a4 = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy + 14),
                                    (lv_coord_t)(sx + 200), (lv_coord_t)(sy + 28)};
                    lv_draw_label(layer, &l4, &a4);
                }
            } else {
                // === CONDENSED ZONE: callsign + alt/speed ===
                const char *cs = ac.callsign[0] ? ac.callsign : ac.icao_hex;
                char alt_str[12];
                if (ac.on_ground) snprintf(alt_str, sizeof(alt_str), "GND");
                else if (ac.altitude >= 18000) snprintf(alt_str, sizeof(alt_str), "FL%d", ac.altitude / 100);
                else snprintf(alt_str, sizeof(alt_str), "%d'", ac.altitude / 100 * 100);

                // Line 1: callsign
                char top[36];
                strlcpy(top, cs, sizeof(top));
                lv_draw_label_dsc_t lbl;
                lv_draw_label_dsc_init(&lbl);
                lbl.color = color;
                lbl.font = &lv_font_montserrat_14;
                lbl.opa = lbl_opa;
                lbl.text = top;
                lv_area_t la1 = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy - 7),
                                  (lv_coord_t)(sx + 200), (lv_coord_t)(sy + 7)};
                lv_draw_label(layer, &lbl, &la1);

                // Line 2: alt + speed (dimmer)
                char info[24];
                snprintf(info, sizeof(info), "%s %dkt", alt_str, ac.speed);
                lv_draw_label_dsc_t il;
                lv_draw_label_dsc_init(&il);
                il.color = color;
                il.font = &lv_font_montserrat_14;
                il.opa = (uint8_t)(lbl_opa * 3 / 4);
                il.text = info;
                lv_area_t la2 = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy + 7),
                                  (lv_coord_t)(sx + 160), (lv_coord_t)(sy + 21)};
                lv_draw_label(layer, &il, &la2);
            }
        }
    }

    _list->unlock();
}

static void draw_filter_label(lv_layer_t *layer) {
    char buf[128];
    lv_color_t color;
    if (filter_label_text(buf, sizeof(buf), &color) == 0) return;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color;
    lbl.font = &lv_font_montserrat_14;
    lbl.opa = LV_OPA_COVER;
    lbl.text = buf;
    lv_area_t la = {(lv_coord_t)8, (lv_coord_t)4,
                    (lv_coord_t)300, (lv_coord_t)20};
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
    _home_list = list;

    _proj.center_lat = g_config.home_lat;
    _proj.center_lon = g_config.home_lon;
    _proj.radius_nm = range_get_nm();

    _radar_obj = lv_obj_create(parent);
    lv_obj_set_size(_radar_obj, RADAR_W, RADAR_H);
    lv_obj_set_pos(_radar_obj, 0, 0);
    lv_obj_set_style_bg_color(_radar_obj, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(_radar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_radar_obj, 0, 0);
    lv_obj_set_style_radius(_radar_obj, 0, 0);
    lv_obj_clear_flag(_radar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_radar_obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    views_attach_swipe(_radar_obj);

    lv_obj_add_event_cb(_radar_obj, radar_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    lv_obj_add_event_cb(_radar_obj, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_RADAR) return;
        if (views_swipe_active()) { views_clear_swipe(); return; }

        if (_filter_just_clicked) {
            _filter_just_clicked = false;
            return;
        }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int tx = point.x;
        int ty = point.y - 30;

        if (detail_card_is_visible()) {
            detail_card_hide();
            return;
        }

        // Hit test (30px radius) -- same filter/hide_ground gating as
        // draw_blips(), so a hidden aircraft can't be tapped.
        if (!_list->lock(pdMS_TO_TICKS(10))) return;
        for (int i = 0; i < _list->count; i++) {
            if (!aircraft_passes_filter(_list->aircraft[i])) continue;
            if (g_config.hide_ground && _list->aircraft[i].on_ground) continue;
            int sx, sy;
            if (to_radar_screen(_list->aircraft[i].lat, _list->aircraft[i].lon, sx, sy)) {
                int dx = tx - sx;
                int dy = ty - sy;
                if (dx * dx + dy * dy < 900) {
                    Aircraft ac_copy = _list->aircraft[i];
                    _list->unlock();
                    detail_card_show(&ac_copy);
                    return;
                }
            }
        }
        _list->unlock();
    }, LV_EVENT_CLICKED, nullptr);

    // Filter toggle buttons — vertical stack on right edge (same layout as
    // map). FILT_VERT gets extra spacing and a thin divider above it -- see
    // map_view.cpp's copy of this loop for why.
    {
        int btn_w = 64, btn_h = 48, btn_gap = 10, group_gap_extra = 14;
        int total_h = NUM_FILTERS * btn_h + (NUM_FILTERS - 1) * btn_gap + group_gap_extra;
        int btn_x = RADAR_W - btn_w - 8;
        int btn_y0 = (RADAR_H - total_h) / 2;
        for (int i = 0; i < NUM_FILTERS; i++) {
            int y = btn_y0 + i * (btn_h + btn_gap) + (i >= FILT_VERT ? group_gap_extra : 0);
            if (i == FILT_VERT) {
                lv_obj_t *div = lv_obj_create(parent);
                lv_obj_set_size(div, btn_w, 1);
                lv_obj_set_pos(div, btn_x, y - (btn_gap + group_gap_extra) / 2);
                lv_obj_set_style_bg_color(div, lv_color_hex(0x444466), 0);
                lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(div, 0, 0);
                lv_obj_set_style_radius(div, 0, 0);
                lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
            }
            lv_obj_t *btn = lv_obj_create(parent);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_set_pos(btn, btn_x, y);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x0a0a1a), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
            lv_obj_set_style_border_color(btn, filter_defs[i].color, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_event_cb(btn, radar_filter_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, filter_defs[i].label);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_center(lbl);

            _filter_btns[i] = btn;
            _filter_lbls[i] = lbl;
        }
    }

    // Clear trails — one-tap, right edge, in the gap between the filter
    // button stack and the bottom of the canvas. Range is now a shared chip
    // in the status bar (status_bar.cpp) -- this view still tracks
    // range_get_nm() itself below to keep _proj.radius_nm in sync.
    {
        lv_obj_t *clr_btn = lv_obj_create(parent);
        lv_obj_set_size(clr_btn, 64, 40);
        lv_obj_set_pos(clr_btn, RADAR_W - 64 - 8, RADAR_H - 28 - 40 - 20);
        lv_obj_set_style_bg_color(clr_btn, lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(clr_btn, LV_OPA_70, 0);
        lv_obj_set_style_border_color(clr_btn, lv_color_hex(0x888899), 0);
        lv_obj_set_style_border_width(clr_btn, 1, 0);
        lv_obj_set_style_border_opa(clr_btn, LV_OPA_40, 0);
        lv_obj_set_style_radius(clr_btn, 6, 0);
        lv_obj_set_style_pad_all(clr_btn, 0, 0);
        lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(clr_btn, [](lv_event_t *e) {
            _filter_just_clicked = true; // reuse the same guard -- prevents the tap-elsewhere handler from also firing
            _trails_cleared_at = millis(); // this view's own cutoff -- doesn't touch shared trail data, so Map is unaffected
            lv_obj_invalidate(_radar_obj);
        }, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *clr_lbl = lv_label_create(clr_btn);
        lv_label_set_text(clr_lbl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(clr_lbl, lv_color_hex(0x888899), 0);
        lv_obj_center(clr_lbl);
    }

    // Animate sweep — always update angle, but only redraw when visible and not touching
    _last_sweep_ms = millis();
    static unsigned _last_synced_filter = ~0u; // impossible bitmask value, forces sync on first tick
    static float _last_range = -1;
    lv_timer_create([](lv_timer_t *t) {
        uint32_t now = millis();
        uint32_t dt = now - _last_sweep_ms;
        _last_sweep_ms = now;
        _sweep_angle += (360.0f * dt) / SWEEP_PERIOD_MS;
        if (_sweep_angle >= 360.0f) _sweep_angle -= 360.0f;

        // Follow the active location (Home or a saved airport) — cheap check,
        // no per-view "on_show" hook exists for Radar so this runs every tick.
        int active = locations_active_index();
        if (active != _last_active_loc) {
            _last_active_loc = active;
            float lat, lon;
            int elev;
            if (locations_get_active_coords(&lat, &lon, &elev)) {
                _proj.center_lat = lat;
                _proj.center_lon = lon;
            }
            _list = locations_active_list(_home_list);
        }

        // Only update when range actually changes
        float r = range_get_nm();
        if (r != _last_range) {
            _last_range = r;
            _proj.radius_nm = r;
        }

        // Sync filter button visuals if filter changed from another view
        unsigned af = filter_get_active();
        if (af != _last_synced_filter) {
            _last_synced_filter = af;
            update_filter_visuals();
        }

        // Skip rendering when touch is active — use global flag (lv_indev_active() is null in timers)
        if (touch_active) return;

        // Only invalidate when radar view is active
        if (views_get_active_index() == VIEW_RADAR) {
            lv_obj_invalidate(_radar_obj);
        }
    }, 100, nullptr);  // 10fps — frees CPU for touch processing
}

void radar_view_update() {
    if (_radar_obj) lv_obj_invalidate(_radar_obj);
}

void radar_view_set_home(float lat, float lon) {
    _proj.center_lat = lat;
    _proj.center_lon = lon;
    if (_radar_obj) lv_obj_invalidate(_radar_obj);
}
