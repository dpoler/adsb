#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// Filter indices — single-select, -1 = show all
#define FILT_NONE     -1
#define FILT_AIRLINE   0
#define FILT_MILITARY  1
#define FILT_EMERGENCY 2
#define FILT_HELI      3
#define FILT_GA        4
#define NUM_FILTERS    5

struct FilterDef {
    const char *label;
    const char *full_name;
    lv_color_t color;
};

// Shared filter definitions (no per-view LVGL pointers — views manage their own buttons)
extern const FilterDef filter_defs[NUM_FILTERS];

// Global active filter state (persists across view switches)
int  filter_get_active();
void filter_set_active(int idx);
void filter_toggle(int idx);  // toggle: if active==idx, set NONE; else set idx

// Filter match logic
bool aircraft_passes_filter(const Aircraft &ac);

// Helpers (also used by map_view icon classification)
bool is_airline_callsign(const char *cs);
bool is_heli_type(const char *t);
