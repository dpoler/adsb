#pragma once
#include "../data/aircraft.h"

struct SessionStats {
    // Live snapshot (recalculated each update)
    int current_count;
    int jets;
    int ga;
    int heli;
    int military;
    int emergency;
    int alt_low;       // < 5000
    int alt_med_low;   // < 15000
    int alt_med;       // < 25000
    int alt_high;      // < 35000
    int alt_very_high; // >= 35000
    char fastest_callsign[9];
    int fastest_speed;
    char slowest_callsign[9];
    int slowest_speed;
    char highest_callsign[9];
    int32_t highest_alt;
    char lowest_callsign[9];
    int32_t lowest_alt;
    char closest_callsign[9];
    float closest_dist;

    // Session totals (accumulated since boot)
    int unique_seen;
    int peak_count;
    uint32_t boot_time;

    // Speed distribution
    int spd_slow;      // < 200kt
    int spd_med;       // < 300kt
    int spd_fast;      // < 400kt
    int spd_very_fast; // < 500kt
    int spd_extreme;   // >= 500kt

    // Top 5 types
    struct TypeCount {
        char type[5];
        int count;
    };
    TypeCount top_types[5];

    // Top 5 airlines (3-letter ICAO prefix)
    struct AirlineCount {
        char code[4];
        int count;
    };
    AirlineCount top_airlines[5];
};

void stats_init();
void stats_update(AircraftList *list);
const SessionStats* stats_get();
