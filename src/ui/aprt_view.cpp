#include <Arduino.h>
#include <math.h>
#include "aprt_view.h"
#include "range.h"
#include "../pins_config.h"

struct RunwayEnd { float lat, lon; };
struct Runway { RunwayEnd le, he; };
struct Airport {
    const char *icao;
    float center_lat, center_lon;
    const Runway *runways;
    int num_runways;
};

static const Runway kden_runways[] = {
    {{39.840944f, -104.726656f}, {39.840657f, -104.683936f}},  // 07/25
    {{39.877556f, -104.662230f}, {39.877244f, -104.619486f}},  // 08/26
    {{39.897036f, -104.686805f}, {39.864371f, -104.687188f}},  // 16L/34R
    {{39.895797f, -104.696085f}, {39.851887f, -104.696588f}},  // 16R/34L
    {{39.864952f, -104.641304f}, {39.832020f, -104.641710f}},  // 17L/35R
    {{39.861245f, -104.660154f}, {39.828313f, -104.660551f}},  // 17R/35L
};

static const Runway klga_runways[] = {
    {{40.769159f, -73.884524f}, {40.785437f, -73.870673f}},    // 04/22
    {{40.782296f, -73.878519f}, {40.772071f, -73.857112f}},    // 13/31
};

static const Airport airports[] = {
    {"KDEN", 39.860027f, -104.673792f, kden_runways, 6},
    {"KLGA", 40.777199f, -73.872597f,  klga_runways, 2},
};
#define NUM_AIRPORTS 2

// Match MAP screen exactly: full content area, same center, same scale formula
#define APRT_W      LCD_H_RES
#define APRT_H      (LCD_V_RES - 30)
#define APRT_CX     (APRT_W / 2)
#define APRT_CY     (APRT_H / 2)
#define SELECTOR_H  48

#define COLOR_RWY           lv_color_hex(0x006600)
#define COLOR_RING          lv_color_hex(0x1a2a3a)
#define COLOR_RANGE_LBL     lv_color_hex(0x00cc33)
#define COLOR_SEL_ACTIVE_BG   lv_color_hex(0x00cc66)
#define COLOR_SEL_ACTIVE_TEXT lv_color_hex(0x000000)
#define COLOR_SEL_IDLE_BG     lv_color_hex(0x1a1a2e)
#define COLOR_SEL_IDLE_TEXT   lv_color_hex(0x888899)
#define COLOR_DRAW_BG       lv_color_hex(0x0a0a1a)

static lv_obj_t *_canvas = nullptr;
static lv_obj_t *_range_label = nullptr;
static lv_obj_t *_sel_btns[NUM_AIRPORTS] = {};
static lv_obj_t *_sel_labels[NUM_AIRPORTS] = {};
static int _sel_idx = 0;

static void draw_rings(lv_layer_t *layer, float radius_nm, float scale) {
    float ring_nm = (radius_nm <= 10) ? 2.0f : (radius_nm <= 25) ? 5.0f : 10.0f;

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = COLOR_RING;
    arc.width = 1;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.center.x = APRT_CX;
    arc.center.y = APRT_CY;

    for (float r = ring_nm; r <= radius_nm; r += ring_nm) {
        arc.radius = (int)(r * scale);
        lv_draw_arc(layer, &arc);
    }
}

static void aprt_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);

    float radius_nm = range_get_nm();
    // Identical scale formula to MAP screen
    float scale = (float)APRT_H / (radius_nm * 2.0f);

    draw_rings(layer, radius_nm, scale);

    const Airport &ap = airports[_sel_idx];
    float cos_lat = cosf(ap.center_lat * (float)M_PI / 180.0f);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_RWY;
    line.width = 1;

    for (int r = 0; r < ap.num_runways; r++) {
        const Runway &rwy = ap.runways[r];

        float dx1 = (rwy.le.lon - ap.center_lon) * 60.0f * cos_lat;
        float dy1 = (rwy.le.lat - ap.center_lat) * 60.0f;
        float dx2 = (rwy.he.lon - ap.center_lon) * 60.0f * cos_lat;
        float dy2 = (rwy.he.lat - ap.center_lat) * 60.0f;

        line.p1 = {(lv_value_precise_t)(APRT_CX + dx1 * scale),
                   (lv_value_precise_t)(APRT_CY - dy1 * scale)};
        line.p2 = {(lv_value_precise_t)(APRT_CX + dx2 * scale),
                   (lv_value_precise_t)(APRT_CY - dy2 * scale)};
        lv_draw_line(layer, &line);
    }
}

static void update_selector_styles() {
    for (int i = 0; i < NUM_AIRPORTS; i++) {
        bool active = (i == _sel_idx);
        lv_obj_set_style_bg_color(_sel_btns[i],
            active ? COLOR_SEL_ACTIVE_BG : COLOR_SEL_IDLE_BG, 0);
        lv_obj_set_style_text_color(_sel_labels[i],
            active ? COLOR_SEL_ACTIVE_TEXT : COLOR_SEL_IDLE_TEXT, 0);
    }
    if (_canvas) lv_obj_invalidate(_canvas);
}

void aprt_view_init(lv_obj_t *parent) {
    // Canvas is full tile size — same as MAP screen so rings use identical scale/center.
    // Selector buttons are created after and render on top (LVGL z-order = creation order).
    _canvas = lv_obj_create(parent);
    lv_obj_set_size(_canvas, APRT_W, APRT_H);
    lv_obj_set_pos(_canvas, 0, 0);
    lv_obj_set_style_bg_color(_canvas, COLOR_DRAW_BG, 0);
    lv_obj_set_style_bg_opa(_canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_canvas, 0, 0);
    lv_obj_set_style_radius(_canvas, 0, 0);
    lv_obj_set_style_pad_all(_canvas, 0, 0);
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(_canvas, aprt_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    // Selector buttons — created after canvas, so they sit above it in z-order
    int btn_w = APRT_W / NUM_AIRPORTS;
    for (int i = 0; i < NUM_AIRPORTS; i++) {
        _sel_btns[i] = lv_obj_create(parent);
        lv_obj_set_size(_sel_btns[i], btn_w, SELECTOR_H);
        lv_obj_set_pos(_sel_btns[i], i * btn_w, 0);
        lv_obj_set_style_radius(_sel_btns[i], 0, 0);
        lv_obj_set_style_border_width(_sel_btns[i], 0, 0);
        lv_obj_set_style_pad_all(_sel_btns[i], 0, 0);
        lv_obj_clear_flag(_sel_btns[i], LV_OBJ_FLAG_SCROLLABLE);

        _sel_labels[i] = lv_label_create(_sel_btns[i]);
        lv_label_set_text(_sel_labels[i], airports[i].icao);
        lv_obj_set_style_text_font(_sel_labels[i], &lv_font_montserrat_20, 0);
        lv_obj_center(_sel_labels[i]);

        lv_obj_add_event_cb(_sel_btns[i], [](lv_event_t *e) {
            _sel_idx = (int)(intptr_t)lv_event_get_user_data(e);
            update_selector_styles();
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // Range label — bottom-right, same position as MAP and RADAR, tappable
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_range_label, COLOR_RANGE_LBL, 0);
    lv_obj_set_pos(_range_label, APRT_W - 80, APRT_H - 28);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
        lv_obj_invalidate(_canvas);
    }, LV_EVENT_CLICKED, nullptr);

    // Poll for range changes from other views
    static float _last_range = -1.0f;
    lv_timer_create([](lv_timer_t *t) {
        float r = range_get_nm();
        if (r != _last_range) {
            _last_range = r;
            if (_range_label) lv_label_set_text(_range_label, range_label());
            if (_canvas) lv_obj_invalidate(_canvas);
        }
    }, 500, nullptr);

    update_selector_styles();
}
