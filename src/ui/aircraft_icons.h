#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"
#include "filters.h"

// Rotate a local-coordinate point by heading (0=north) around origin, translate to screen pos
static inline void rotate_pt(float lx, float ly, float sin_h, float cos_h,
                              int cx, int cy, lv_point_precise_t &out) {
    out.x = (lv_value_precise_t)(cx + lx * cos_h - ly * sin_h);
    out.y = (lv_value_precise_t)(cy + lx * sin_h + ly * cos_h);
}

// s=1.0 is MAP-default size; scale down for zoomed-out APRT ranges
static inline void draw_tri(lv_layer_t *layer, int cx, int cy, float sin_h, float cos_h,
                             float x0, float y0, float x1, float y1, float x2, float y2,
                             lv_color_t color, uint8_t opa, float s = 1.0f) {
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = opa;
    rotate_pt(x0*s, y0*s, sin_h, cos_h, cx, cy, tri.p[0]);
    rotate_pt(x1*s, y1*s, sin_h, cos_h, cx, cy, tri.p[1]);
    rotate_pt(x2*s, y2*s, sin_h, cos_h, cx, cy, tri.p[2]);
    lv_draw_triangle(layer, &tri);
}

static inline void draw_icon_airliner(lv_layer_t *layer, int cx, int cy,
                                      float sin_h, float cos_h, lv_color_t color, uint8_t opa,
                                      float s = 1.0f) {
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-18,  1.5f,0,  -1.5f,0,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-18,  1.5f,0,  0,18,     color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-18, -1.5f,0,  0,18,     color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-3,  14,2,  0,4,          color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-3, -14,2,  0,4,          color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,13,   6,17,  0,18,        color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,13,  -6,17,  0,18,        color, opa, s);
}

static inline void draw_icon_jet(lv_layer_t *layer, int cx, int cy,
                                 float sin_h, float cos_h, lv_color_t color, uint8_t opa,
                                 float s = 1.0f) {
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-8,  1.5f,4, -1.5f,4,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-8,  1.5f,4,  0,8,     color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-8, -1.5f,4,  0,8,     color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-2,   9,4,   0,3,      color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-2,  -9,4,   0,3,      color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,5,    4,8,   0,8,      color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,5,   -4,8,   0,8,      color, opa, s);
}

static inline void draw_icon_ga(lv_layer_t *layer, int cx, int cy,
                                float sin_h, float cos_h, lv_color_t color, uint8_t opa,
                                float s = 1.0f) {
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-5,  1,0,  -1,0,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  1,0,  -1,0,   0,5,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-2,  8,-1,  0,0,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-2, -8,-1,  0,0,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,3,   3,5,  -3,5,  color, opa, s);
}

static inline void draw_icon_heli(lv_layer_t *layer, int cx, int cy,
                                  float sin_h, float cos_h, lv_color_t color, uint8_t opa,
                                  float s = 1.0f) {
    draw_tri(layer, cx, cy, sin_h, cos_h,  0,-5,  3,1,  -3,1,  color, opa, s);
    draw_tri(layer, cx, cy, sin_h, cos_h,  3,1,  -3,1,   0,6,  color, opa, s);
    lv_draw_line_dsc_t rotor;
    lv_draw_line_dsc_init(&rotor);
    rotor.color = color;
    rotor.width = 1;
    rotor.opa = (uint8_t)(opa * 3 / 4);
    lv_point_precise_t r0, r1, r2, r3;
    rotate_pt(-7*s,-2*s, sin_h, cos_h, cx, cy, r0);
    rotate_pt( 7*s,-2*s, sin_h, cos_h, cx, cy, r1);
    rotate_pt( 0,-8*s,   sin_h, cos_h, cx, cy, r2);
    rotate_pt( 0, 4*s,   sin_h, cos_h, cx, cy, r3);
    rotor.p1 = {r0.x, r0.y}; rotor.p2 = {r1.x, r1.y};
    lv_draw_line(layer, &rotor);
    rotor.p1 = {r2.x, r2.y}; rotor.p2 = {r3.x, r3.y};
    lv_draw_line(layer, &rotor);
}

enum IconType { ICON_AIRLINER, ICON_JET, ICON_GA, ICON_HELI };

static inline IconType classify_icon(const Aircraft &ac) {
    if (ac.category[0] == 'A' && ac.category[1] == '7') return ICON_HELI;
    if (ac.type_code[0] && is_heli_type(ac.type_code)) return ICON_HELI;
    if (ac.is_military) return ICON_JET;
    if (is_airline_callsign(ac.callsign)) return ICON_AIRLINER;
    if (ac.category[0] == 'A' && ac.category[1] >= '3') return ICON_AIRLINER;
    return ICON_GA;
}
