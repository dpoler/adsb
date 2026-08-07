#include "metar.h"
#include "http_mutex.h"
#include "locations.h"
#include "../ui/geo.h" // MapProjection::distance_nm()
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cmath>

// Same reasoning as fetcher.cpp/enrichment.cpp/ota.cpp: NetworkClientSecure's
// default TLS handshake timeout (120s) isn't bounded by
// HTTPClient::setTimeout() (that only covers the read phase after a
// connection succeeds).
#define TLS_HANDSHAKE_TIMEOUT_S 8

// aviationweather.gov's own usage guidance asks for <=1 request/min per
// client and recommends caching. METARs themselves only update ~hourly (more
// often around a significant change), so this leaves huge margin -- also
// re-fetches immediately on an active-location change (see metar_poll()) so
// switching locations doesn't leave a stale reading up for up to this long.
#define METAR_REFRESH_MS (10UL * 60UL * 1000UL)

// Search radius around the active location. Tested empirically against a
// real foothills-adjacent point near Denver: a 10nm box came back with zero
// stations, a 20nm box found three (13-16nm away). Wide enough to reliably
// find something in most areas, tight enough that "no station in range"
// stays a real, reachable outcome rather than never happening. bbox is a
// rectangle, not a circle, so its corners reach ~1.4x this -- every
// candidate is re-checked against the true radius below rather than trusting
// the box edges.
#define METAR_RANGE_NM 20.0f

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[192] = "";
char metar_station[8] = "";

// PSRAM allocator for ArduinoJson -- same pattern as fetcher.cpp/
// locations.cpp/enrichment.cpp/ota.cpp, keeps internal RAM free for SDIO/
// WiFi buffers.
struct MetarPsramAlloc : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* p) override {
        heap_caps_free(p);
    }
    void* reallocate(void* p, size_t size) override {
        return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM);
    }
};
static MetarPsramAlloc _metar_alloc;

static void do_fetch(float lat, float lon) {
    metar_status = METAR_FETCHING;

    // Degrees-per-NM only varies with latitude for longitude, not latitude
    // itself -- same formula MapProjection::to_screen() (geo.h) uses.
    float dlat = METAR_RANGE_NM / 60.0f;
    float dlon = METAR_RANGE_NM / (60.0f * cosf(lat * (float)M_PI / 180.0f));

    char url[192];
    snprintf(url, sizeof(url),
        "https://aviationweather.gov/api/data/metar?bbox=%.4f,%.4f,%.4f,%.4f&format=json",
        lat - dlat, lon - dlon, lat + dlat, lon + dlon);

    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) {
        metar_status = METAR_ERROR;
        Serial.println("[METAR] Fetch failed -- network busy, try again");
        return;
    }
    {
        WiFiClientSecure client;
        client.setInsecure(); // matches this app's other third-party HTTPS calls
        client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        HTTPClient http;
        http.begin(client, url);
        // aviationweather.gov's usage policy asks API consumers to set a
        // custom User-Agent so its traffic-filtering doesn't mistake this
        // for abuse -- same reasoning as ota.cpp's GitHub User-Agent header.
        http.addHeader("User-Agent", "adsb-display");
        http.setTimeout(10000);

        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            JsonDocument doc(&_metar_alloc);
            DeserializationError err = deserializeJson(doc, http.getStream());
            if (!err) {
                float best_dist = 1e9f;
                const char *best_raw = nullptr;
                const char *best_id = nullptr;
                for (JsonObject station : doc.as<JsonArray>()) {
                    const char *raw = station["rawOb"] | "";
                    if (!raw[0]) continue;
                    float slat = station["lat"] | 0.0f;
                    float slon = station["lon"] | 0.0f;
                    float d = MapProjection::distance_nm(lat, lon, slat, slon);
                    if (d <= METAR_RANGE_NM && d < best_dist) {
                        best_dist = d;
                        best_raw = raw;
                        best_id = station["icaoId"] | "";
                    }
                }
                if (best_raw) {
                    strlcpy(metar_raw, best_raw, sizeof(metar_raw));
                    strlcpy(metar_station, best_id, sizeof(metar_station));
                    metar_status = METAR_OK;
                    Serial.printf("[METAR] %s (%.1fnm)\n", metar_station, (double)best_dist);
                } else {
                    metar_raw[0] = '\0';
                    metar_station[0] = '\0';
                    metar_status = METAR_NO_STATION;
                    Serial.println("[METAR] No station within range");
                }
            } else {
                metar_status = METAR_ERROR;
                Serial.println("[METAR] JSON parse error");
            }
        } else {
            metar_status = METAR_ERROR;
            Serial.printf("[METAR] Fetch failed -- HTTP %d\n", code);
        }
        http.end();
    }
    http_mutex_release();
}

void metar_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2; // impossible sentinel -- forces a fetch on the very first tick

    int idx = locations_active_index();
    if (idx == -1) {
        if (last_loc_idx != -1) {
            metar_status = METAR_IDLE;
            metar_raw[0] = '\0';
            metar_station[0] = '\0';
            last_loc_idx = -1;
        }
        return;
    }

    uint32_t now = millis();
    bool loc_changed = (idx != last_loc_idx);
    bool due = (now - last_fetch_ms >= METAR_REFRESH_MS);
    if (!loc_changed && !due) return;

    float lat, lon;
    if (!locations_get_active_coords(&lat, &lon, nullptr)) return;

    // On a location change, blank the old reading immediately -- before
    // do_fetch() below, which can take real seconds (DNS/TLS/HTTP round
    // trip). Without this, the previous location's METAR stays globally
    // visible (and stats_view.cpp keeps drawing it) for however long the
    // new fetch takes, reading as a stale/wrong value rather than "still
    // loading" -- reported after switching away from KJFK and seeing its
    // METAR linger on screen. stats_view.cpp does its own independent
    // blank-on-change too (its own poll loop runs on a different task/timer
    // than this one, so it can't just wait for this to land either).
    if (loc_changed) {
        metar_raw[0] = '\0';
        metar_station[0] = '\0';
    }

    last_loc_idx = idx;
    last_fetch_ms = now;
    do_fetch(lat, lon);
}
