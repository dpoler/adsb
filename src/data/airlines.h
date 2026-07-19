#pragma once

// ICAO airline code -> full name lookup, derived from a flight's callsign
// prefix. Data source: dpoler/AirlinesCSV on GitHub — a small dedicated repo
// shared with the author's other project (originally lived in that other
// project's own repo; moved out once a second project started consuming it,
// so the shared-data dependency is explicit rather than hidden). See
// airlines.csv there for the format and how to add/edit entries. Airline
// code-to-name mapping is essentially static aviation reference data (unlike
// route/schedule data, it doesn't go stale the same way — see
// project_route_data memory for why that distinction mattered enough to
// remove the old route feature).
#define AIRLINES_MAX 250

struct AirlineEntry {
    char code[4];      // ICAO airline code, e.g. "UAL"
    char name[26];     // e.g. "United Airlines"
    char callsign[16]; // telephony callsign, e.g. "UNITED" (optional, may be empty)
};

// Fetch and parse airlines.csv. Blocking network call — call once from an
// existing background task at boot, not the UI thread and not a new task
// (see project_p4_heap_constraints memory). Returns true on success.
bool airlines_load();

// Look up the airline for a full flight callsign (e.g. "UAL1234") by
// extracting its 2-3 letter ICAO prefix. Returns nullptr if not found or
// airlines_load() hasn't completed yet.
const AirlineEntry *airline_lookup(const char *callsign);
