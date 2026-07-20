#include "filters.h"
#include "aircraft_icons.h" // classify_icon() -- reused below for FILT_GA
#include <cstring>

const FilterDef filter_defs[NUM_FILTERS] = {
    {"COM",  "COMMERCIAL",  lv_color_hex(0x4488ff)},
    {"MIL",  "MILITARY",    lv_color_hex(0xffaa00)},
    {"EMG",  "EMERGENCY",   lv_color_hex(0xff3333)},
    {"HELI", "HELICOPTERS", lv_color_hex(0x44ddaa)},
    {"GA",   "GENERAL AVIATION", lv_color_hex(0x44dd44)},
};

static int _active_filter = FILT_NONE;

int filter_get_active() { return _active_filter; }
void filter_set_active(int idx) { _active_filter = idx; }

void filter_toggle(int idx) {
    if (_active_filter == idx)
        _active_filter = FILT_NONE;
    else
        _active_filter = idx;
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
    if (_active_filter == FILT_NONE) return true;

    switch (_active_filter) {
        case FILT_AIRLINE:
            if (is_airline_callsign(ac.callsign)) return true;
            if (ac.category[0] == 'A' && ac.category[1] >= '3') return true;
            return false;
        case FILT_MILITARY:
            return ac.is_military;
        case FILT_EMERGENCY:
            return ac.is_emergency;
        case FILT_HELI:
            if (ac.category[0] == 'A' && ac.category[1] == '7') return true;
            if (ac.type_code[0] && is_heli_type(ac.type_code)) return true;
            return false;
        case FILT_GA:
            // Reuses the same classification the map icon/legend already use
            // (aircraft_icons.h) rather than a separate GA heuristic.
            return classify_icon(ac) == ICON_GA;
    }
    return true;
}
