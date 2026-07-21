#include <Arduino.h>
#include "stats.h"
#include "geo.h"
#include "../data/storage.h"
#include "../data/locations.h"
#include <cstring>
#include <cstdlib>

static SessionStats _stats;

// Simple hash set for unique ICAO tracking
#define MAX_UNIQUE 2000
static char _seen_icaos[MAX_UNIQUE][7];
static int _seen_count = 0;

// unique_seen/peak_count/top_types/top_airlines are meant to describe
// whichever location is currently selected (see stats_view.cpp's per-column
// global-vs-per-location split) -- without this, switching from Home to a
// saved airport (or back) kept accumulating into the same global counters,
// so e.g. "UNIQUE" silently became "unique aircraft across every location
// you've ever viewed since boot" instead of "unique aircraft at the one
// you're looking at now". Reset on location change; boot_time/UPTIME stays
// genuinely global and is not reset here.
static int _last_active_loc = -2; // sentinel so the very first call resets/syncs

static bool already_seen(const char *icao) {
    for (int i = 0; i < _seen_count; i++) {
        if (strcmp(_seen_icaos[i], icao) == 0) return true;
    }
    return false;
}

static void mark_seen(const char *icao) {
    if (_seen_count >= MAX_UNIQUE || already_seen(icao)) return;
    strlcpy(_seen_icaos[_seen_count], icao, 7);
    _seen_count++;
}

static bool is_airline_callsign(const char *cs) {
    return cs[0] >= 'A' && cs[0] <= 'Z' &&
           cs[1] >= 'A' && cs[1] <= 'Z' &&
           cs[2] >= 'A' && cs[2] <= 'Z' &&
           cs[3] >= '0' && cs[3] <= '9';
}

static bool is_heli_type(const char *t) {
    static const char *types[] = {
        "R22", "R44", "R66", "EC35", "EC45", "EC55",
        "A109", "A139", "A169", "B06", "B212", "B412",
        "S76", "S92", "B407", "B429", "B505",
        "H135", "H145", "H160", "H175", "H225",
        "AS50", "AS55", "AS65", "MD52", "MD60",
        "NH90", "CH47", "V22", "UH1", "BK17",
        nullptr
    };
    for (int i = 0; types[i]; i++) {
        if (strcmp(t, types[i]) == 0) return true;
    }
    return false;
}

#define MAX_TYPE_TRACK 100
struct TypeTracker {
    char type[5];
    int count;
};
static TypeTracker _type_counts[MAX_TYPE_TRACK];
static int _type_track_count = 0;

#define MAX_AIRLINE_TRACK 100
struct AirlineTracker {
    char code[4];
    int count;
};
static AirlineTracker _airline_counts[MAX_AIRLINE_TRACK];
static int _airline_track_count = 0;

static void track_type(const char *type) {
    if (!type[0]) return;
    for (int i = 0; i < _type_track_count; i++) {
        if (strcmp(_type_counts[i].type, type) == 0) {
            _type_counts[i].count++;
            return;
        }
    }
    if (_type_track_count < MAX_TYPE_TRACK) {
        strlcpy(_type_counts[_type_track_count].type, type, 5);
        _type_counts[_type_track_count].count = 1;
        _type_track_count++;
    }
}

static void track_airline(const char *callsign) {
    if (!is_airline_callsign(callsign)) return;
    char code[4] = {callsign[0], callsign[1], callsign[2], '\0'};
    for (int i = 0; i < _airline_track_count; i++) {
        if (strcmp(_airline_counts[i].code, code) == 0) {
            _airline_counts[i].count++;
            return;
        }
    }
    if (_airline_track_count < MAX_AIRLINE_TRACK) {
        strlcpy(_airline_counts[_airline_track_count].code, code, 4);
        _airline_counts[_airline_track_count].count = 1;
        _airline_track_count++;
    }
}

static void compute_top_types() {
    for (int i = 0; i < 5 && i < _type_track_count; i++) {
        int max_idx = i;
        for (int j = i + 1; j < _type_track_count; j++) {
            if (_type_counts[j].count > _type_counts[max_idx].count) max_idx = j;
        }
        if (max_idx != i) {
            TypeTracker tmp = _type_counts[i];
            _type_counts[i] = _type_counts[max_idx];
            _type_counts[max_idx] = tmp;
        }
        strlcpy(_stats.top_types[i].type, _type_counts[i].type, 5);
        _stats.top_types[i].count = _type_counts[i].count;
    }
}

static void compute_top_airlines() {
    for (int i = 0; i < 5 && i < _airline_track_count; i++) {
        int max_idx = i;
        for (int j = i + 1; j < _airline_track_count; j++) {
            if (_airline_counts[j].count > _airline_counts[max_idx].count) max_idx = j;
        }
        if (max_idx != i) {
            AirlineTracker tmp = _airline_counts[i];
            _airline_counts[i] = _airline_counts[max_idx];
            _airline_counts[max_idx] = tmp;
        }
        strlcpy(_stats.top_airlines[i].code, _airline_counts[i].code, 4);
        _stats.top_airlines[i].count = _airline_counts[i].count;
    }
}

void stats_init() {
    memset(&_stats, 0, sizeof(_stats));
    _stats.boot_time = millis();
    _stats.closest_dist = 9999.0f;
    _seen_count = 0;
    _type_track_count = 0;
    _airline_track_count = 0;
}

void stats_update(AircraftList *list) {
    if (!list->lock(pdMS_TO_TICKS(50))) return;

    int active_loc = locations_active_index();
    if (active_loc != _last_active_loc) {
        _last_active_loc = active_loc;
        _seen_count = 0;
        _type_track_count = 0;
        _airline_track_count = 0;
        _stats.peak_count = 0;
        _stats.unique_seen = 0;
        memset(_stats.top_types, 0, sizeof(_stats.top_types));
        memset(_stats.top_airlines, 0, sizeof(_stats.top_airlines));
    }

    _stats.current_count = 0;
    _stats.jets = 0;
    _stats.ga = 0;
    _stats.heli = 0;
    _stats.military = 0;
    _stats.emergency = 0;
    _stats.alt_gnd = 0;
    _stats.alt_low = 0;
    _stats.alt_med_low = 0;
    _stats.alt_med = 0;
    _stats.alt_high = 0;
    _stats.alt_very_high = 0;
    _stats.fastest_speed = 0;
    _stats.fastest_callsign[0] = 0;
    _stats.slowest_speed = 99999;
    _stats.slowest_callsign[0] = 0;
    _stats.highest_alt = -9999;
    _stats.highest_callsign[0] = 0;
    _stats.lowest_alt = 999999;
    _stats.lowest_callsign[0] = 0;
    _stats.closest_dist = 9999.0f;
    _stats.closest_callsign[0] = 0;
    _stats.spd_gnd = 0;
    _stats.spd_slow = 0;
    _stats.spd_med = 0;
    _stats.spd_fast = 0;
    _stats.spd_very_fast = 0;
    _stats.spd_extreme = 0;

    // CLOSEST should measure from wherever is actually being viewed, not
    // always Home -- falls back to Home's coords if the active location was
    // somehow removed out from under us (locations_get_active_coords()
    // leaves these untouched on failure).
    float ref_lat = g_config.home_lat, ref_lon = g_config.home_lon;
    locations_get_active_coords(&ref_lat, &ref_lon, nullptr);

    uint32_t now = millis();

    for (int i = 0; i < list->count; i++) {
        Aircraft &ac = list->aircraft[i];
        if (compute_aircraft_opacity(ac.stale_since, now) == 0) continue;
        if (ac.lat == 0 && ac.lon == 0) continue;

        _stats.current_count++;
        bool is_new = !already_seen(ac.icao_hex);
        mark_seen(ac.icao_hex);
        if (is_new) {
            track_type(ac.type_code);
            track_airline(ac.callsign);
        }

        // Speed distribution
        if (ac.on_ground) _stats.spd_gnd++;
        else if (ac.speed < 200) _stats.spd_slow++;
        else if (ac.speed < 300) _stats.spd_med++;
        else if (ac.speed < 400) _stats.spd_fast++;
        else if (ac.speed < 500) _stats.spd_very_fast++;
        else _stats.spd_extreme++;

        if (ac.is_military) _stats.military++;
        if (ac.is_emergency) _stats.emergency++;

        bool is_h = (ac.category[0] == 'A' && ac.category[1] == '7') ||
                    (ac.type_code[0] && is_heli_type(ac.type_code));
        bool is_j = !is_h && (is_airline_callsign(ac.callsign) ||
                   (ac.category[0] == 'A' && ac.category[1] >= '3'));

        if (is_h) _stats.heli++;
        else if (is_j) _stats.jets++;
        else _stats.ga++;

        if (ac.on_ground) _stats.alt_gnd++;
        else if (ac.altitude < 5000) _stats.alt_low++;
        else if (ac.altitude < 15000) _stats.alt_med_low++;
        else if (ac.altitude < 25000) _stats.alt_med++;
        else if (ac.altitude < 35000) _stats.alt_high++;
        else _stats.alt_very_high++;

        // Fastest (any aircraft with speed data)
        if (ac.speed > _stats.fastest_speed) {
            _stats.fastest_speed = ac.speed;
            strlcpy(_stats.fastest_callsign,
                    ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
        }

        // Airborne-only records: filter out on_ground and zero/bogus values
        if (!ac.on_ground && ac.speed > 30) {
            // Slowest airborne (>30kt filters out parked/taxiing with bad on_ground)
            if (ac.speed < _stats.slowest_speed) {
                _stats.slowest_speed = ac.speed;
                strlcpy(_stats.slowest_callsign,
                        ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
            }
        }
        if (!ac.on_ground && ac.altitude > 100) {
            // Highest
            if (ac.altitude > _stats.highest_alt) {
                _stats.highest_alt = ac.altitude;
                strlcpy(_stats.highest_callsign,
                        ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
            }
            // Lowest airborne (>100ft filters out just-landed with bad on_ground)
            if (ac.altitude < _stats.lowest_alt) {
                _stats.lowest_alt = ac.altitude;
                strlcpy(_stats.lowest_callsign,
                        ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
            }
        }

        float d = MapProjection::distance_nm(ref_lat, ref_lon, ac.lat, ac.lon);
        if (d < _stats.closest_dist) {
            _stats.closest_dist = d;
            strlcpy(_stats.closest_callsign,
                    ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
        }
    }

    if (_stats.current_count > _stats.peak_count) {
        _stats.peak_count = _stats.current_count;
    }
    _stats.unique_seen = _seen_count;
    compute_top_types();
    compute_top_airlines();

    list->unlock();
}

const SessionStats* stats_get() {
    return &_stats;
}
