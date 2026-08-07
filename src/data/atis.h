#pragma once

// Digital ATIS (D-ATIS) text for the active airport, via datis.clowd.io
// (FAA SWIM-backed, no API key). US airports first; some fields publish
// separate arrival/departure ATIS (e.g. KDEN) — both are kept when present.
//
// Driven by atis_poll() the same way metar_poll() is (rate-limited; safe
// every tick). Waypoints / locations without an ICAO stay ATIS_IDLE.

#include <stdint.h>

enum AtisStatus {
    ATIS_IDLE,      // no airport ICAO active, or nothing fetched yet
    ATIS_FETCHING,
    ATIS_OK,
    ATIS_NONE,     // fetch ok, airport has no D-ATIS
    ATIS_ERROR,
};

#define ATIS_MAX_REPORTS 2
#define ATIS_TEXT_LEN    1600
#define ATIS_TYPE_LEN    12
#define ATIS_CODE_LEN    4

struct AtisReport {
    char type[ATIS_TYPE_LEN]; // "combined" | "arr" | "dep"
    char code[ATIS_CODE_LEN]; // info letter, e.g. "A"
    char text[ATIS_TEXT_LEN];
};

extern volatile AtisStatus atis_status;
extern AtisReport atis_reports[ATIS_MAX_REPORTS];
extern int atis_report_count; // 0..ATIS_MAX_REPORTS when ATIS_OK

void atis_poll();
