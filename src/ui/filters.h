#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// Filter indices — bit positions in the active-filter bitmask. Order here is
// also the on-screen button order (top to bottom): COM, GA, HELI, MIL, EMG,
// then VERT/HIGH/LOW below the divider (all three are altitude/motion
// *states*, not alternative categories -- see FILTER_STATE_MASK in
// filters.cpp).
#define FILT_AIRLINE   0
#define FILT_GA        1
#define FILT_HELI      2
#define FILT_MILITARY  3
#define FILT_EMERGENCY 4
#define FILT_VERT      5
#define FILT_HIGH      6
#define FILT_LOW       7
#define NUM_FILTERS    8

struct FilterDef {
    const char *label;
    const char *full_name;
    lv_color_t color;
};

// Shared filter definitions (no per-view LVGL pointers — views manage their own buttons)
extern const FilterDef filter_defs[NUM_FILTERS];

// Active filter state (bitmask) for whichever of Map/Radar/Arrivals(List)
// is currently active -- each remembers its own selection independently
// (g_config.view_filter_mask, storage.h; resolved via
// views_filterable_index(), views.h). Any number of filters can be active
// at once -- an aircraft passes if it matches ANY active filter (OR); with
// none active, everything passes. No init step needed -- g_config is
// already loaded from NVS by the time anything calls these.
unsigned filter_get_active();       // bitmask, e.g. (1u<<FILT_GA)|(1u<<FILT_HELI)
void     filter_toggle(int idx);    // flips bit idx for the active view, persists immediately

// Builds "FILTER: X" / "FILTER: X + Y" for whichever filters are currently
// active into buf, and sets *color to the first active filter's color (a
// single accent color, not blended across multiple active filters). Returns
// the number of active filters -- 0 means none active, caller should skip
// drawing the label in that case.
int filter_label_text(char *buf, size_t buf_size, lv_color_t *color);

// Filter match logic
bool aircraft_passes_filter(const Aircraft &ac);

// Helpers (also used by map_view icon classification)
bool is_airline_callsign(const char *cs);
bool is_heli_type(const char *t);
