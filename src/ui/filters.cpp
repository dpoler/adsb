#include "filters.h"
#include "aircraft_icons.h" // classify_icon() -- reused below for FILT_GA
#include "../data/locations.h" // locations_get_active_coords() -- reused below for FILT_VERT's AGL check
#include <cstring>
#include <cstdio>

const FilterDef filter_defs[NUM_FILTERS] = {
    {"COM",  "COMMERCIAL",  lv_color_hex(0x00bbff)}, // matches COLOR_COMMERCIAL (geo.h) -- the actual on-screen airliner icon color
    {"MIL",  "MILITARY",    lv_color_hex(0xffaa00)},
    {"EMG",  "EMERGENCY",   lv_color_hex(0xff3333)},
    {"HELI", "HELICOPTERS", lv_color_hex(0xdd44ff)}, // matches COLOR_HELI_CAT (geo.h) -- the actual on-screen heli icon color
    {"GA",   "GENERAL AVIATION", lv_color_hex(0x44dd44)},
    {"VERT", "ASCENDING/DESCENDING", lv_color_hex(0x22ccdd)},
};

static unsigned _active_mask = 0;

unsigned filter_get_active() { return _active_mask; }

void filter_toggle(int idx) {
    _active_mask ^= (1u << idx);
}

int filter_label_text(char *buf, size_t buf_size, lv_color_t *color) {
    if (buf_size) buf[0] = '\0';
    size_t pos = 0;
    int count = 0;
    for (int i = 0; i < NUM_FILTERS; i++) {
        if (!(_active_mask & (1u << i))) continue;
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

bool aircraft_passes_filter(const Aircraft &ac) {
    if (_active_mask == 0) return true;

    if ((_active_mask & (1u << FILT_AIRLINE)) &&
        (is_airline_callsign(ac.callsign) || (ac.category[0] == 'A' && ac.category[1] >= '3')))
        return true;
    if ((_active_mask & (1u << FILT_MILITARY)) && ac.is_military)
        return true;
    if ((_active_mask & (1u << FILT_EMERGENCY)) && ac.is_emergency)
        return true;
    if ((_active_mask & (1u << FILT_HELI)) &&
        ((ac.category[0] == 'A' && ac.category[1] == '7') ||
         (ac.type_code[0] && is_heli_type(ac.type_code))))
        return true;
    if ((_active_mask & (1u << FILT_GA)) && classify_icon(ac) == ICON_GA)
        // Reuses the same classification the map icon/legend already use
        // (aircraft_icons.h) rather than a separate GA heuristic.
        return true;
    if ((_active_mask & (1u << FILT_VERT)) && !ac.on_ground && ac.vert_rate_valid) {
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
        const int32_t AGL_CEILING_FT = 10000;
        int elev_ft = 0;
        locations_get_active_coords(nullptr, nullptr, &elev_ft);
        int32_t agl = ac.altitude - elev_ft;
        if (agl <= AGL_CEILING_FT && (ac.vert_rate > 300 || ac.vert_rate < -300))
            return true;
    }

    return false;
}
