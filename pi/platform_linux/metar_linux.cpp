// Pi port of src/data/metar.cpp — same aviationweather.gov public Data API
// (no key), same nearest-station-within-20nm selection. Uses
// platform_http_get() instead of ESP32 HTTPClient/PSRAM ArduinoJson.

#include "../../src/data/metar.h"
#include "../../src/data/locations.h"
#include "../../src/platform/platform.h"
#include "../../src/ui/geo.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#define METAR_REFRESH_MS (10UL * 60UL * 1000UL)
#define METAR_RANGE_NM 20.0f

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[192] = "";
char metar_station[8] = "";

static void do_fetch(float lat, float lon) {
    metar_status = METAR_FETCHING;

    float dlat = METAR_RANGE_NM / 60.0f;
    float cos_lat = cosf(lat * (float)M_PI / 180.0f);
    if (fabsf(cos_lat) < 1e-4f) cos_lat = 1e-4f;
    float dlon = METAR_RANGE_NM / (60.0f * cos_lat);

    char url[220];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/metar?bbox=%.4f,%.4f,%.4f,%.4f&format=json",
             lat - dlat, lon - dlon, lat + dlat, lon + dlon);

    // Response can be several KB for a dense metro bbox.
    constexpr size_t BUF = 48 * 1024;
    char *body = new (std::nothrow) char[BUF];
    if (!body) {
        metar_status = METAR_ERROR;
        platform_log("[METAR] OOM\n");
        return;
    }
    size_t len = 0;
    bool ok = platform_http_get(url, body, BUF, &len);
    if (!ok || len == 0) {
        delete[] body;
        metar_status = METAR_ERROR;
        platform_log("[METAR] Fetch failed\n");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, len);
    delete[] body;
    if (err) {
        metar_status = METAR_ERROR;
        platform_log("[METAR] JSON parse error: %s\n", err.c_str());
        return;
    }

    float best_dist = 1e9f;
    const char *best_raw = nullptr;
    const char *best_id = nullptr;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) {
        metar_status = METAR_ERROR;
        platform_log("[METAR] unexpected JSON shape\n");
        return;
    }
    for (JsonObject station : arr) {
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
        strlcpy(metar_station, best_id ? best_id : "", sizeof(metar_station));
        metar_status = METAR_OK;
        platform_log("[METAR] %s (%.1fnm)\n", metar_station, (double)best_dist);
    } else {
        metar_raw[0] = '\0';
        metar_station[0] = '\0';
        metar_status = METAR_NO_STATION;
        platform_log("[METAR] No station within range\n");
    }
}

void metar_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2;

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

    uint32_t now = platform_millis();
    bool loc_changed = (idx != last_loc_idx);
    bool due = (now - last_fetch_ms >= METAR_REFRESH_MS);
    if (!loc_changed && !due) return;

    float lat, lon;
    if (!locations_get_active_coords(&lat, &lon, nullptr)) return;

    if (loc_changed) {
        metar_raw[0] = '\0';
        metar_station[0] = '\0';
    }

    last_loc_idx = idx;
    last_fetch_ms = now;
    do_fetch(lat, lon);
}
