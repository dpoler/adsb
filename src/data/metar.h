#pragma once

// Nearest-station METAR lookup via aviationweather.gov's public Data API
// (no key, no auth -- https://aviationweather.gov/data/api/). Always
// searches by lat/lon (bbox=) rather than querying the active location's
// own ICAO directly, even for airports -- a saved airport having an ICAO
// doesn't mean it reports its own weather (most small GA fields don't have
// an ASOS/AWOS at all), so "nearest reporting station within range" is the
// one code path that's actually correct for every location type (airport
// or plain waypoint).
//
// Driven by metar_poll(), called from location_poll_task's existing loop
// (fetcher.cpp) -- not a dedicated FreeRTOS task, same reasoning as
// ota_poll()/locations_nearby_poll() (project_p4_heap_constraints memory:
// new tasks doing network work already crashed this board's SDIO driver
// once). Internally rate-limited (see metar.cpp) -- safe to call every tick.

enum MetarStatus {
    METAR_IDLE,       // no active location, or nothing fetched yet
    METAR_FETCHING,
    METAR_OK,
    METAR_NO_STATION, // fetch succeeded, nothing reporting within range
    METAR_ERROR,      // network/HTTP/parse failure -- metar_raw keeps its last good value
};

extern volatile MetarStatus metar_status;
extern char metar_raw[192];   // winning station's raw METAR text, verbatim
extern char metar_station[8]; // which ICAO metar_raw actually came from (may differ from the active location's own ICAO)

void metar_poll();
