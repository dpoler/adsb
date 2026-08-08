#pragma once

// ICAO ↔ name helpers over the generated airports_db[] (OurAirports
// large/medium). Safe to call when the DB header is missing — lookups
// return nullptr / 0 matches.

#if __has_include("airports_db.h")
#include "airports_db.h"
#elif __has_include("ui/airports_db.h")
#include "ui/airports_db.h"
#else
// Stub so call sites compile without regenerating the DB.
struct StaticAirport {
    char icao[5];
    char name[64];
    float lat, lon;
    unsigned char large;
};
#endif

// Exact ICAO match (case-insensitive). nullptr if unknown / DB absent.
const StaticAirport *airports_lookup_icao(const char *icao);

// Case-insensitive substring match on name/ICAO. Writes up to max_out
// pointers into out[]; returns the number written. Exact ICAO hits first.
int airports_search(const char *query, const StaticAirport **out, int max_out);

// Convenience: copy truncated name for an ICAO into buf, or "" if unknown.
void airports_format_name(const char *icao, char *buf, int buf_size);
