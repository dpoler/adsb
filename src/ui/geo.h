#pragma once
#include <cmath>
#include "lvgl.h"

// Convert nautical miles to approximate degrees of latitude
#define NM_TO_DEG_LAT (1.0f / 60.0f)

struct MapProjection {
    float center_lat;
    float center_lon;
    float radius_nm;    // visible radius in nautical miles
    int screen_w;
    int screen_h;
    int offset_x;       // for panning
    int offset_y;
    int top_margin = 0; // reserves clearance at the screen's top edge (e.g.
                         // so range rings don't touch chrome directly above
                         // the canvas) by shrinking the effective drawing
                         // height and recentering down instead of scaling to
                         // touch y=0. 0 = old behavior, edge to edge.

    // Convert lat/lon to screen x,y. Returns false if off-screen.
    bool to_screen(float lat, float lon, int &sx, int &sy) const {
        float dx_nm = (lon - center_lon) * 60.0f * cosf(center_lat * M_PI / 180.0f);
        float dy_nm = (lat - center_lat) * 60.0f;

        float scale = (float)(screen_h - top_margin) / (radius_nm * 2.0f);
        sx = (int)(screen_w / 2 + dx_nm * scale) + offset_x;
        sy = (int)(screen_h / 2 + top_margin / 2 - dy_nm * scale) + offset_y;

        return (sx >= -20 && sx < screen_w + 20 && sy >= -20 && sy < screen_h + 20);
    }

    // Distance in nautical miles between two points (Haversine)
    static float distance_nm(float lat1, float lon1, float lat2, float lon2) {
        float dlat = (lat2 - lat1) * M_PI / 180.0f;
        float dlon = (lon2 - lon1) * M_PI / 180.0f;
        float a = sinf(dlat / 2) * sinf(dlat / 2) +
                  cosf(lat1 * M_PI / 180.0f) * cosf(lat2 * M_PI / 180.0f) *
                  sinf(dlon / 2) * sinf(dlon / 2);
        float c = 2.0f * atan2f(sqrtf(a), sqrtf(1 - a));
        return c * 3440.065f; // Earth radius in NM
    }
};

// HSL -> RGB, H in degrees [0,360), S/L as fractions [0,1].
static inline void hsl_to_rgb(float h, float s, float l, uint8_t *r, uint8_t *g, uint8_t *b) {
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float rp, gp, bp;
    if      (hp < 1) { rp = c; gp = x; bp = 0; }
    else if (hp < 2) { rp = x; gp = c; bp = 0; }
    else if (hp < 3) { rp = 0; gp = c; bp = x; }
    else if (hp < 4) { rp = 0; gp = x; bp = c; }
    else if (hp < 5) { rp = x; gp = 0; bp = c; }
    else             { rp = c; gp = 0; bp = x; }
    *r = (uint8_t)((rp + m) * 255.0f + 0.5f);
    *g = (uint8_t)((gp + m) * 255.0f + 0.5f);
    *b = (uint8_t)((bp + m) * 255.0f + 0.5f);
}

// Altitude to color for trails. Hue ramp (orange -> yellow -> green ->
// magenta -> red) and altitude breakpoints ported from the ADS-B community's
// de facto standard altitude coloring -- originally FlightAware's
// skyaware/dump1090-fa, now used by tar1090, readsb, and most self-hosted
// ADS-B viewers -- rather than an arbitrary palette, so this display's
// altitude colors read the same way other ADS-B tools' do. Source:
// https://github.com/wiedehopf/tar1090 html/defaults.js, ColorByAlt.air.
// Simplified from the source: saturation/lightness are held flat here
// instead of also varying by hue (the source's `l` table exists to keep
// perceived brightness consistent across hues -- a refinement not worth the
// extra lookup table for a small embedded trail line).
static inline lv_color_t altitude_color(int32_t alt_ft) {
    if (alt_ft <= 0) return lv_color_hex(0x666666); // ground

    static const struct { float alt, hue; } pts[] = {
        {0,     20.0f},  // orange
        {2000,  32.5f},
        {4000,  43.0f},  // yellow
        {6000,  54.0f},
        {8000,  72.0f},
        {9000,  85.0f},  // green-yellow
        {11000, 140.0f}, // light green
        {40000, 300.0f}, // magenta
        {51000, 360.0f}, // red
    };
    const int n = sizeof(pts) / sizeof(pts[0]);
    float hue = pts[0].hue;
    for (int i = n - 1; i >= 0; i--) {
        if (alt_ft > pts[i].alt) {
            hue = (i == n - 1) ? pts[i].hue
                : pts[i].hue + (pts[i + 1].hue - pts[i].hue) *
                      (alt_ft - pts[i].alt) / (pts[i + 1].alt - pts[i].alt);
            break;
        }
    }

    uint8_t r, g, b;
    hsl_to_rgb(hue, 0.88f, 0.47f, &r, &g, &b);
    return lv_color_make(r, g, b);
}

// Aircraft category colors — distinctive per type
#define COLOR_COMMERCIAL  lv_color_hex(0x00bbff)  // cyan-blue
#define COLOR_MILITARY    lv_color_hex(0xffaa00)  // amber/gold
#define COLOR_GA_PRIVATE  lv_color_hex(0x44dd44)  // bright green
#define COLOR_HELI_CAT    lv_color_hex(0xdd44ff)  // magenta/purple
#define COLOR_EMERGENCY   lv_color_hex(0xff3333)  // red
