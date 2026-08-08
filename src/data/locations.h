#pragma once
#include <cstddef>
#include "aircraft.h"

#define MAX_LOCATIONS 15   // saved locations (airports + waypoints)
#define MAX_RUNWAYS   12   // KORD has exactly 8 active runways (11 total, 3
                            // closed) -- was capped at 8, right at the edge;
                            // bumped for headroom now that closed runways are
                            // filtered out during parsing (locations.cpp)
#define LOC_ICAO_LEN  8
#define LOC_NAME_LEN  17   // 16 usable chars + null -- picker rows are narrow;
                            // airports auto-name themselves after their ICAO,
                            // waypoints need a user-chosen name to identify
                            // them at all (no ICAO to fall back on)

// Safety ceiling for cached nearby LARGE airports per location (see the
// nearby-runways declarations below). Large-only rarely approaches this even
// in dense metro areas (KJFK at 50nm has ~20 airports total, but only a
// handful are classified `large_airport` -- JFK/LGA/EWR-class) -- this just
// bounds the pathological case.
#define NEARBY_MAX 20

struct LocRunway {
    float le_lat, le_lon;
    float he_lat, he_lon;
    char le_id[4];
    char he_id[4];
};

// A saved location is either an airport or a plain waypoint -- there is no
// other kind, and no separately-tracked "Home". icao[0] == '\0' is the
// discriminator: empty means "this is a waypoint" (lat/lon/elevation entered
// directly by the user, no runway geometry, never attempts an airportdb.io
// fetch). Airports have runways[]/runway_count from airportdb.io as before.
// Every location -- airport or waypoint -- has a `name` used for display;
// for airports it's auto-set to the ICAO on add, for waypoints the user
// supplies it (it's the only thing identifying the row in the picker).
struct Location {
    char icao[LOC_ICAO_LEN];
    char name[LOC_NAME_LEN];
    float lat, lon;
    int elevation_ft;
    LocRunway runways[MAX_RUNWAYS];
    int runway_count;
    bool nearby_enabled; // "show nearby large airports' runways" toggle
    int nearby_count;    // cached count -- cheap to read for any row's badge
                          // without loading the actual runway data (see
                          // locations_nearby_count())
};

// Load saved locations from NVS. Call once at boot.
void locations_init();

int locations_count();                     // number of saved locations
const Location* locations_get(int idx);    // idx in [0, locations_count())

// Fetch runway/elevation data for `icao` from airportdb.io and persist it as
// a new airport-type location. Blocking network call — call from a
// background task, not the UI thread. Returns true on success; on failure,
// if err is non-null, writes a short reason.
bool locations_add_from_icao(const char *icao, char *err, size_t err_size);

// Adds a plain waypoint -- no network fetch, so this is synchronous and safe
// to call directly from the UI thread (unlike locations_add_from_icao(),
// there's nothing to poll for). name is truncated to LOC_NAME_LEN-1 and has
// any '|' stripped (the serial-config protocol's field delimiter -- see
// serial_config.cpp). Returns true on success; on failure (name empty, or
// the location list is full), if err is non-null, writes a short reason.
bool locations_add_waypoint(const char *name, float lat, float lon, int elevation_ft,
                             char *err, size_t err_size);

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

// Live token check -- same request/poll/result shape and same reason as the
// "add by ICAO" trio above (a real fetch_airport_data() call blocks on an
// HTTPS round trip; serial_config_poll() runs on the main render loop, not
// location_poll_task, so this can't be called directly from a serial
// command handler without freezing the display for however long the
// request takes). Tests the currently-saved token against a fixed,
// always-present ICAO and discards the result -- this only tells you
// whether the token *authenticates*, not anything about the test airport
// itself. ok=false with "no token set" if the field is empty; doesn't
// attempt a network call in that case.
void locations_request_verify_token();
void locations_verify_token_poll();
bool locations_verify_token_result(bool *ok, char *err, size_t err_size);

void locations_remove(int idx);

// Moves the location at `from` to position `to`, shifting everything between
// them by one slot (same semantics as a list drag-to-reorder). No-op if
// either index is out of range or they're equal. The active selection (if
// any) is remapped to keep pointing at the same location, not the same slot.
void locations_reorder(int from, int to);

// Currently selected location. -1 = none selected (empty list, or nothing
// chosen yet -- e.g. right after a factory reset). Callers must handle this
// as a real "no active location" state, not assume there's always a
// fallback -- there isn't one anymore.
int locations_active_index();
void locations_set_active(int idx);

// Convenience — resolves the active selection into a lat/lon/elevation
// triple. Returns false if nothing is active (see locations_active_index()).
bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft);

// "Nearby large airports" cache: draws full runway geometry (not just a
// glyph) for large airports near a saved location, on top of that location's
// own runways. Works identically for airport- and waypoint-type locations --
// the original motivating case was a plain waypoint dropped between several
// nearby airports. Large airports only (from the static DB's `large` flag)
// -- medium airports stay glyph-only. A dense 50nm radius (e.g. KJFK) can
// have ~20 airports total, but rarely more than a handful of LARGE ones,
// which is both the cheaper set to fetch and the one worth a full runway
// diagram at a glance.
//
// Fetched once, at the moment the toggle is first turned on -- same
// fetch-and-cache-at-a-discrete-action reasoning as locations_add_from_icao()
// itself, not lazily as airports scroll into view, so it never needs to
// re-fetch across zoom levels. Only the *active* location's cache is ever
// resident in DRAM at once (loaded from NVS on demand) -- embedding it
// inline in all MAX_LOCATIONS slots at all times would multiply DRAM use by
// 15x for data that's only ever drawn for whichever one location is active.
bool locations_nearby_enabled(int idx);
int locations_nearby_count(int idx);                  // cheap: persisted header only, safe for any row (e.g. the picker's "+N nearby" badge)
void locations_nearby_set_enabled(int idx, bool on);   // turning on triggers a fetch if nothing's cached yet; turning off just stops drawing it (cached data stays on disk)

// Currently active location's cached nearby-airport list (lazily (re)loaded
// from NVS whenever the active location changes). Empty if the toggle is off
// or nothing's been fetched yet.
const Location* locations_nearby_get_active(int *count);

// Poll counterpart to locations_add_poll() -- call from the same
// location_poll_task loop. Drains one queued nearby-airport fetch per call.
void locations_nearby_poll();

// Drops every location's cached nearby-airport runway list (NVS blobs on
// ESP32, embedded "nearby" arrays on Pi) without deleting the locations
// themselves or clearing the eye-toggle. Count goes to 0 so the next
// off→on (or an explicit re-kick) re-fetches. Used by Pi Settings
// "Clear all caches".
void locations_nearby_cache_clear();

// Erases the entire "adsb_locs" NVS namespace -- every saved location and
// every nearby-runways cache blob (nb_<name> keys live in this same
// namespace, so a full clear takes those with it too, no separate cleanup
// needed). Does not touch in-memory state -- caller is expected to reboot
// immediately (serial_config.cpp's FACTORY_RESET does), at which point
// locations_init() picks up the now-empty namespace on next boot.
// Pi (locations_linux.cpp) also clears the in-memory tables and deletes
// ~/.config/flightlevel314/locations.json so a reboot is not required there.
void locations_factory_reset();
