#pragma once
#include <cstddef>
#include "aircraft.h"

#define MAX_LOCATIONS 15   // saved airports, not counting Home
#define MAX_RUNWAYS   8
#define LOC_ICAO_LEN  8

struct LocRunway {
    float le_lat, le_lon;
    float he_lat, he_lon;
    char le_id[4];
    char he_id[4];
};

struct Location {
    char icao[LOC_ICAO_LEN];
    float lat, lon;
    int elevation_ft;
    LocRunway runways[MAX_RUNWAYS];
    int runway_count;
};

// Home is not stored here — it's the existing g_config.home_lat/home_lon,
// treated as the implicit location at index -1 everywhere in this API.

// Load saved airports from NVS. Call once at boot.
void locations_init();

int locations_count();                     // number of saved (non-home) airports
const Location* locations_get(int idx);    // idx in [0, locations_count())

// Fetch runway/elevation data for `icao` from airportdb.io and persist it.
// Blocking network call — call from a background task, not the UI thread.
// Returns true on success; on failure, if err is non-null, writes a short reason.
bool locations_add_from_icao(const char *icao, char *err, size_t err_size);

// Request/response pair for adding a location from the UI thread without
// spawning a new task — a dedicated task's stack was enough extra internal-DRAM
// pressure to crash the SDIO driver on this board (see
// project_p4_heap_constraints memory). Instead: the UI calls
// locations_request_add(), and locations_add_poll() — called from
// location_poll_task's existing loop in fetcher.cpp — picks it up and does the
// actual fetch on that task's already-allocated stack.
void locations_request_add(const char *icao);
void locations_add_poll();
// Non-blocking: returns true (and clears the pending result) if an add
// completed since the last call. *ok/err are only valid when this returns true.
bool locations_add_result(bool *ok, char *err, size_t err_size);

void locations_remove(int idx);

// Currently selected location. -1 = Home.
int locations_active_index();
void locations_set_active(int idx);

// Convenience — resolves the active selection (Home or a saved Location) into
// a lat/lon/elevation triple. Returns false if idx is out of range.
bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft);

// Resolves the active selection into whichever AircraftList currently holds
// its data: `home_list` when Home is active, or the shared on-demand list
// (fetcher_location_list()) when a saved airport is active. Views should call
// this each time they need the list, rather than caching the result — the
// active location can change out from under them at any time via the picker.
AircraftList* locations_active_list(AircraftList *home_list);
