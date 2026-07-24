#include "filters.h"
#include "aircraft_icons.h" // classify_icon() -- reused below for FILT_GA
#include "views.h" // views_filterable_index() -- Map/Radar/Arrivals each get an independent filter mask
#include "../data/locations.h" // locations_get_active_coords() -- reused below for FILT_VERT's AGL check
#include "../data/storage.h" // g_config.view_filter_mask -- per-view persistence
#include <cstring>
#include <cstdio>

const FilterDef filter_defs[NUM_FILTERS] = {
    {"COM",  "COMMERCIAL",  lv_color_hex(0x00bbff)}, // matches COLOR_COMMERCIAL (geo.h) -- the actual on-screen airliner icon color
    {"GA",   "GENERAL AVIATION", lv_color_hex(0x44dd44)},
    {"HELI", "HELICOPTERS", lv_color_hex(0xdd44ff)}, // matches COLOR_HELI_CAT (geo.h) -- the actual on-screen heli icon color
    {"MIL",  "MILITARY",    lv_color_hex(0xffaa00)},
    {"EMG",  "EMERGENCY",   lv_color_hex(0xff3333)},
    {"VERT", "ASCENDING/DESCENDING", lv_color_hex(0x669999)},
    {"HIGH", "HIGH ALTITUDE", lv_color_hex(0x77aaee)},
    {"LOW",  "LOW ALTITUDE",  lv_color_hex(0xcc9966)},
};

// Masks off any stray high bits from a stale/corrupt NVS value rather than
// trusting it outright.
static unsigned current_mask() {
    return g_config.view_filter_mask[views_filterable_index()] & ((1u << NUM_FILTERS) - 1);
}

unsigned filter_get_active() { return current_mask(); }

void filter_toggle(int idx) {
    int v = views_filterable_index();
    g_config.view_filter_mask[v] ^= (1u << idx);

    // HIGH and LOW describe the same altitude band and can't both be true of
    // one aircraft -- turning one on turns the other off, same mutual
    // exclusion as VERT/GND (though that pair spans two separate config
    // fields and three duplicated view files; both bits here already live in
    // the same mask, so it's one check in one place instead).
    unsigned &mask = g_config.view_filter_mask[v];
    if (idx == FILT_HIGH && (mask & (1u << FILT_HIGH))) mask &= ~(1u << FILT_LOW);
    else if (idx == FILT_LOW && (mask & (1u << FILT_LOW))) mask &= ~(1u << FILT_HIGH);

    // Every call site is a discrete button tap (never a high-frequency path
    // like a slider drag), so persisting immediately is safe -- see the
    // trail-length slider's cyan-flash fix for why that distinction matters.
    storage_save_config(g_config);
}

int filter_label_text(char *buf, size_t buf_size, lv_color_t *color) {
    unsigned mask = current_mask();
    if (buf_size) buf[0] = '\0';
    size_t pos = 0;
    int count = 0;
    for (int i = 0; i < NUM_FILTERS; i++) {
        if (!(mask & (1u << i))) continue;
        if (count == 0 && color) *color = filter_defs[i].color;
        const char *sep = (count == 0) ? "FILTER: " : " + ";
        int n = snprintf(buf + pos, pos < buf_size ? buf_size - pos : 0,
                          "%s%s", sep, filter_defs[i].full_name);
        if (n > 0) pos += (size_t)n;
        count++;
    }
    return count;
}

bool is_airline_callsign(const char *cs) {
    return cs[0] >= 'A' && cs[0] <= 'Z' &&
           cs[1] >= 'A' && cs[1] <= 'Z' &&
           cs[2] >= 'A' && cs[2] <= 'Z' &&
           cs[3] >= '0' && cs[3] <= '9';
}

bool is_heli_type(const char *t) {
    static const char *heli_types[] = {
        "R22", "R44", "R66", "EC35", "EC45", "EC55",
        "A109", "A139", "A169", "B06", "B212", "B412",
        "S76", "S92", "B407", "B429", "B505",
        "H135", "H145", "H160", "H175", "H225",
        "AS50", "AS55", "AS65", "MD52", "MD60",
        "NH90", "CH47", "V22", "UH1", "BK17",
        nullptr
    };
    for (int i = 0; heli_types[i]; i++) {
        if (strcmp(t, heli_types[i]) == 0) return true;
    }
    return false;
}

// Categories (COM/MIL/EMG/HELI/GA) are alternative classifications of an
// aircraft -- picking several means "show any of these" (OR-within-group;
// a group with nothing active passes automatically). States (VERT/HIGH/LOW)
// are different: each active one is an independent condition an aircraft
// must ALSO satisfy (AND-across-bits), since they describe orthogonal
// properties of the SAME aircraft rather than alternatives to pick from --
// VERT+HIGH means "ascending/descending AND above the altitude band", not
// "ascending/descending OR high" (which would just be a stranger way of
// picking two things independently, defeating the point of combining them).
// HIGH and LOW are mutually exclusive by construction (filter_toggle()) --
// they describe the same axis (altitude band), so there's never an "OR
// between them" question to begin with.
#define FILTER_CATEGORY_MASK \
    ((1u << FILT_AIRLINE) | (1u << FILT_MILITARY) | (1u << FILT_EMERGENCY) | \
     (1u << FILT_HELI) | (1u << FILT_GA))
#define FILTER_STATE_MASK ((1u << FILT_VERT) | (1u << FILT_HIGH) | (1u << FILT_LOW))

// Shared AGL split for both VERT's "close enough to the ground for a climb/
// descend to mean landing/departing" ceiling and HIGH/LOW's altitude-band
// threshold -- 10,000ft AGL is a common, recognizable aviation reference
// point (matches dpoler/FlightRadarCYD's acColor(), see passes_state_filters()
// below), and using one constant for both keeps "high" and "not near the
// ground" consistent with each other rather than picking two similar but
// separately-tuned numbers.
static const int32_t AGL_BAND_FT = 10000;

static bool passes_category_filters(const Aircraft &ac, unsigned bits) {
    if ((bits & (1u << FILT_AIRLINE)) &&
        (is_airline_callsign(ac.callsign) || (ac.category[0] == 'A' && ac.category[1] >= '3')))
        return true;
    if ((bits & (1u << FILT_MILITARY)) && ac.is_military)
        return true;
    if ((bits & (1u << FILT_EMERGENCY)) && ac.is_emergency)
        return true;
    if ((bits & (1u << FILT_HELI)) &&
        ((ac.category[0] == 'A' && ac.category[1] == '7') ||
         (ac.type_code[0] && is_heli_type(ac.type_code))))
        return true;
    if ((bits & (1u << FILT_GA)) && classify_icon(ac) == ICON_GA)
        // Reuses the same classification the map icon/legend already use
        // (aircraft_icons.h) rather than a separate GA heuristic.
        return true;
    return false;
}

// Each active bit here is an independent condition the aircraft must ALSO
// satisfy (AND-across-bits, unlike passes_category_filters()'s OR) -- see
// the FILTER_STATE_MASK comment above for why. Returns false the moment any
// active condition fails; true only once every active one has held (with
// none active, there's nothing to narrow by, so it passes vacuously).
static bool passes_state_filters(const Aircraft &ac, unsigned bits) {
    int elev_ft = 0;
    locations_get_active_coords(nullptr, nullptr, &elev_ft);
    int32_t agl = ac.altitude - elev_ft;

    if (bits & (1u << FILT_VERT)) {
        // "Landing/departing" traffic, not just anything with a nonzero
        // vertical rate -- a cruise-altitude step-climb would otherwise
        // match too. Same AGL-ceiling approach as dpoler/FlightRadarCYD's
        // acColor() (10,000ft AGL there, using its own per-location
        // elevation setting); reuses this project's existing +-300fpm
        // climb/descend convention (detail_card.cpp, radar_view.cpp,
        // arrivals_view.cpp's status_from_vert_rate()) instead of that
        // project's +-2m/s.
        //
        // vert_rate_valid gate matters: the feed doesn't report baro_rate
        // every cycle for every aircraft, and the parser defaults a missing
        // value to 0 (see fetcher.cpp) -- without this check, an aircraft
        // that's genuinely climbing/descending but whose most recent update
        // simply omitted baro_rate would read as "level" and drop out of
        // this filter, even though nothing about its actual flight changed.
        if (ac.on_ground || !ac.vert_rate_valid) return false;
        if (!(agl <= AGL_BAND_FT && (ac.vert_rate > 300 || ac.vert_rate < -300))) return false;
    }
    // HIGH/LOW both exclude ground traffic explicitly -- GND (a separate,
    // unconditional exclude -- see storage.h) already owns "hide aircraft on
    // the ground"; without this, an on-ground aircraft's ~0 AGL would
    // otherwise always match LOW, conflating "low-flying" with "not flying".
    if (bits & (1u << FILT_HIGH)) {
        if (ac.on_ground || !(agl > AGL_BAND_FT)) return false;
    }
    if (bits & (1u << FILT_LOW)) {
        if (ac.on_ground || !(agl <= AGL_BAND_FT)) return false;
    }
    return true;
}

bool aircraft_passes_filter(const Aircraft &ac) {
    unsigned mask = current_mask();
    unsigned category_bits = mask & FILTER_CATEGORY_MASK;
    unsigned state_bits = mask & FILTER_STATE_MASK;

    if (category_bits && !passes_category_filters(ac, category_bits)) return false;
    if (state_bits && !passes_state_filters(ac, state_bits)) return false;
    return true;
}
