#pragma once
#include <cstdint>

struct AircraftEnrichment {
    char photo_url[256];
    char photo_photographer[48];
    char manufacturer[32];
    char model[48];
    char owner[48];
    char registered_country[24];
    char engine_type[24];
    uint8_t engine_count;
    uint16_t year_built;
    bool loaded;
    bool loading;
};

// Initialize enrichment system (installs LVGL timer for deferred callbacks)
void enrichment_init();

// Fetch enrichment data in background. Calls callback progressively as data arrives.
// Callback is always called from LVGL context (safe to update UI).
void enrichment_fetch(const char *icao_hex, const char *registration,
                      void (*callback)(AircraftEnrichment *data));

// Get cached enrichment (returns nullptr if not yet fetched)
AircraftEnrichment *enrichment_get_cached(const char *icao_hex);
