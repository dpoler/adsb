#include "locations.h"
#include "storage.h"
#include "http_mutex.h"
#include "fetcher.h"
#include "../ui/range.h"
#include "lvgl.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cctype>

// PSRAM allocator for the airportdb.io JSON response — keeps this off internal
// DRAM, which this board (ESP32-P4 + C6 co-processor) already runs thin on.
struct PsramAllocator : ArduinoJson::Allocator {
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
static PsramAllocator _psram_alloc;

static Preferences _prefs;
static Location _locations[MAX_LOCATIONS];
static int _count = 0;
static int _active_index = -1; // -1 = Home

static void location_sync_timer_cb(lv_timer_t *t);

// "Add by ICAO" request/response — processed by locations_add_poll(), called
// from location_poll_task's existing loop rather than a dedicated task.
static SemaphoreHandle_t _add_mutex = nullptr;
static bool _add_pending = false;
static char _add_pending_icao[LOC_ICAO_LEN] = {};
static bool _add_result_ready = false;
static bool _add_result_ok = false;
static char _add_result_err[48] = {};

// On-disk format: for each saved location, a fixed-size header (icao, lat,
// lon, elevation_ft, runway_count) followed by exactly runway_count
// LocRunway entries -- NOT a fixed MAX_RUNWAYS reservation. A location's
// runways[] array in memory is sized for the worst case (KORD-sized
// airports), but writing that full reservation to NVS for every saved
// airport regardless of how many runways it actually has is exactly the
// kind of large blocking flash write that visibly stalls this board's LCD
// panel (see project_p4_heap_constraints memory -- the cyan-flash bug).
// Packing tightly keeps the write proportional to real data.
struct LocationHeader {
    char icao[LOC_ICAO_LEN];
    float lat, lon;
    int elevation_ft;
    int runway_count;
};

static void save_all() {
    _prefs.begin("adsb_locs", false);
    _prefs.putInt("count", _count);
    if (_count > 0) {
        size_t buf_size = 0;
        for (int i = 0; i < _count; i++)
            buf_size += sizeof(LocationHeader) + (size_t)_locations[i].runway_count * sizeof(LocRunway);

        uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t pos = 0;
            for (int i = 0; i < _count; i++) {
                const Location &loc = _locations[i];
                LocationHeader hdr;
                strlcpy(hdr.icao, loc.icao, sizeof(hdr.icao));
                hdr.lat = loc.lat;
                hdr.lon = loc.lon;
                hdr.elevation_ft = loc.elevation_ft;
                hdr.runway_count = loc.runway_count;
                memcpy(buf + pos, &hdr, sizeof(hdr));
                pos += sizeof(hdr);
                size_t rwy_bytes = (size_t)loc.runway_count * sizeof(LocRunway);
                memcpy(buf + pos, loc.runways, rwy_bytes);
                pos += rwy_bytes;
            }
            _prefs.putBytes("locs", buf, buf_size);
            heap_caps_free(buf);
        }
    } else {
        _prefs.remove("locs");
    }
    _prefs.end();
}

void locations_init() {
    _prefs.begin("adsb_locs", true);
    _count = _prefs.getInt("count", 0);
    if (_count < 0) _count = 0;
    if (_count > MAX_LOCATIONS) _count = MAX_LOCATIONS;
    memset(_locations, 0, sizeof(_locations));

    size_t blob_len = _prefs.getBytesLength("locs");
    Serial.printf("[locations] init: stored count=%d, blob_len=%u\n", _count, (unsigned)blob_len);
    if (_count > 0 && blob_len > 0) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(blob_len, MALLOC_CAP_SPIRAM);
        int parsed = 0;
        if (buf) {
            size_t got = _prefs.getBytes("locs", buf, blob_len);
            size_t pos = 0;
            for (; parsed < _count; parsed++) {
                if (pos + sizeof(LocationHeader) > got) break; // truncated -- bail, don't trust the rest
                LocationHeader hdr;
                memcpy(&hdr, buf + pos, sizeof(hdr));
                pos += sizeof(hdr);
                if (hdr.runway_count < 0 || hdr.runway_count > MAX_RUNWAYS) break; // corrupt
                size_t rwy_bytes = (size_t)hdr.runway_count * sizeof(LocRunway);
                if (pos + rwy_bytes > got) break; // truncated

                Location &loc = _locations[parsed];
                strlcpy(loc.icao, hdr.icao, sizeof(loc.icao));
                loc.lat = hdr.lat;
                loc.lon = hdr.lon;
                loc.elevation_ft = hdr.elevation_ft;
                loc.runway_count = hdr.runway_count;
                memcpy(loc.runways, buf + pos, rwy_bytes);
                pos += rwy_bytes;
            }
            heap_caps_free(buf);
        }
        if (parsed != _count) {
            // Inconsistent/corrupt/old-format blob -- don't trust partial data.
            Serial.printf("[locations] init: parsed only %d of %d claimed locations -- resetting to empty\n", parsed, _count);
            _count = 0;
            memset(_locations, 0, sizeof(_locations));
        } else {
            for (int i = 0; i < _count; i++) {
                Serial.printf("[locations] init: loaded[%d] = %s, runway_count=%d\n", i, _locations[i].icao, _locations[i].runway_count);
            }
        }
    } else if (_count > 0) {
        // count > 0 but no blob at all -- inconsistent, reset.
        Serial.printf("[locations] init: count=%d but no blob -- resetting to empty\n", _count);
        _count = 0;
    }

    _prefs.end();
    _active_index = -1; // always boot on Home

    _add_mutex = xSemaphoreCreateMutex();

    lv_timer_create(location_sync_timer_cb, 1000, nullptr);
}

int locations_count() {
    return _count;
}

const Location* locations_get(int idx) {
    if (idx < 0 || idx >= _count) return nullptr;
    return &_locations[idx];
}

void locations_remove(int idx) {
    if (idx < 0 || idx >= _count) return;
    for (int i = idx; i < _count - 1; i++) {
        _locations[i] = _locations[i + 1];
    }
    _count--;
    memset(&_locations[_count], 0, sizeof(Location));
    if (_active_index == idx) _active_index = -1;
    else if (_active_index > idx) _active_index--;
    save_all();
}

int locations_active_index() {
    return _active_index;
}

void locations_set_active(int idx) {
    if (idx < -1 || idx >= _count) return;
    _active_index = idx;
}

bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft) {
    if (_active_index == -1) {
        if (lat) *lat = g_config.home_lat;
        if (lon) *lon = g_config.home_lon;
        if (elevation_ft) *elevation_ft = g_config.home_elevation_ft;
        return true;
    }
    const Location *loc = locations_get(_active_index);
    if (!loc) return false;
    if (lat) *lat = loc->lat;
    if (lon) *lon = loc->lon;
    if (elevation_ft) *elevation_ft = loc->elevation_ft;
    return true;
}

AircraftList* locations_active_list(AircraftList *home_list) {
    return (_active_index == -1) ? home_list : fetcher_location_list();
}

// Keeps the on-demand secondary fetch targeted at whichever non-home location
// is active, using the currently selected range as the query radius. Runs
// regardless of which of the 4 views is on screen — any of them may be
// showing a saved airport's data.
static void location_sync_timer_cb(lv_timer_t *t) {
    if (_active_index == -1) {
        fetcher_set_location_target(0, 0, 0); // Home is covered by the main fetch loop
        return;
    }
    const Location *loc = locations_get(_active_index);
    if (!loc) return;
    fetcher_set_location_target(loc->lat, loc->lon, (int)range_get_nm());
}

// NOTE: airportdb.io wraps OurAirports data. Field names below follow
// OurAirports' well-known airports.csv/runways.csv column names
// (latitude_deg/longitude_deg/elevation_ft, le_ident/le_latitude_deg/...).
// Not verified against a live response in this environment (no network
// access while writing this) — if fields come back zeroed/missing on real
// hardware, check the actual response shape and adjust the keys below.
bool locations_add_from_icao(const char *icao, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!icao || !icao[0]) return fail("no ICAO given");
    if (_count >= MAX_LOCATIONS) return fail("location list full");
    if (!g_config.airportdb_token[0]) return fail("no airportdb.io token set");

    char icao_upper[LOC_ICAO_LEN] = {};
    strlcpy(icao_upper, icao, sizeof(icao_upper));
    for (char *p = icao_upper; *p; p++) *p = toupper((unsigned char)*p);

    // Check for an existing entry first — avoid duplicate network calls.
    Serial.printf("[locations] add %s: _count=%d before dedup check\n", icao_upper, _count);
    for (int i = 0; i < _count; i++) {
        Serial.printf("[locations]   existing[%d] = %s\n", i, _locations[i].icao);
        if (strcmp(_locations[i].icao, icao_upper) == 0) {
            Serial.printf("[locations] %s already in saved list -- skipping fetch, existing (possibly stale) entry kept\n", icao_upper);
            return fail("already saved");
        }
    }

    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) return fail("network busy, try again");

    char url[160];
    snprintf(url, sizeof(url), "https://airportdb.io/api/v1/airport/%s?apiToken=%s",
             icao_upper, g_config.airportdb_token);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(10000);
    int code = http.GET();

    bool ok = false;
    if (code == HTTP_CODE_OK) {
        int len = http.getSize();
        size_t buf_size = (len > 0) ? (size_t)len + 1 : 64 * 1024;
        char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t total = 0;
            size_t target = (len > 0) ? (size_t)len : buf_size - 1;
            WiFiClient *stream = http.getStreamPtr();
            uint32_t deadline = millis() + 10000;
            while (total < target && millis() < deadline) {
                int avail = stream->available();
                if (avail > 0) {
                    int to_read = min((size_t)avail, target - total);
                    total += stream->readBytes(buf + total, to_read);
                } else if (!stream->connected()) {
                    break;
                } else {
                    vTaskDelay(1);
                }
            }
            buf[total] = '\0';

            if (total > 0) {
                JsonDocument doc(&_psram_alloc);
                if (!deserializeJson(doc, buf, total)) {
                    // airportdb.io returns every coordinate as a JSON *string*
                    // (e.g. "39.8409"), not a number — inherited from its CSV
                    // pipeline. Use .as<float>() (does string->number
                    // conversion) rather than the `| default` operator (which
                    // only converts when the stored type already matches).
                    float lat = doc["latitude_deg"].as<float>();
                    float lon = doc["longitude_deg"].as<float>();
                    if (lat != 0.0f || lon != 0.0f) {
                        Location loc = {};
                        strlcpy(loc.icao, icao_upper, sizeof(loc.icao));
                        loc.lat = lat;
                        loc.lon = lon;
                        loc.elevation_ft = doc["elevation_ft"].as<int>();

                        JsonArray rwys = doc["runways"].as<JsonArray>();
                        Serial.printf("[locations] %s: %d raw runway entries in response\n", icao_upper, (int)rwys.size());
                        for (JsonObject r : rwys) {
                            const char *le_id_dbg = r["le_ident"] | "?";
                            const char *he_id_dbg = r["he_ident"] | "?";
                            const char *closed_raw = r["closed"] | "?";
                            int closed_parsed = r["closed"].as<int>();
                            if (loc.runway_count >= MAX_RUNWAYS) {
                                Serial.printf("[locations]   %s/%s: SKIP (MAX_RUNWAYS cap hit)\n", le_id_dbg, he_id_dbg);
                                break;
                            }
                            // Skip decommissioned runways -- OurAirports (which
                            // airportdb.io wraps) tracks this per-runway (e.g.
                            // KORD's old diagonals 14L/32R, 15/33, 18/36 are
                            // marked closed=1 despite still having valid
                            // coordinates) and without this check we'd draw
                            // them as if active, and -- since MAX_RUNWAYS is a
                            // fixed cap -- potentially crowd out a real active
                            // runway that arrives later in the array.
                            if (closed_parsed == 1) {
                                Serial.printf("[locations]   %s/%s: closed=\"%s\" (parsed %d) -- SKIPPED\n", le_id_dbg, he_id_dbg, closed_raw, closed_parsed);
                                continue;
                            }
                            float le_lat = r["le_latitude_deg"].as<float>();
                            float le_lon = r["le_longitude_deg"].as<float>();
                            float he_lat = r["he_latitude_deg"].as<float>();
                            float he_lon = r["he_longitude_deg"].as<float>();
                            // Skip runways OurAirports has no threshold coordinates for —
                            // draw only what we can actually place on the map.
                            if ((le_lat == 0.0f && le_lon == 0.0f) ||
                                (he_lat == 0.0f && he_lon == 0.0f)) {
                                Serial.printf("[locations]   %s/%s: closed=\"%s\" (parsed %d) -- KEPT but zero coords, SKIPPED\n", le_id_dbg, he_id_dbg, closed_raw, closed_parsed);
                                continue;
                            }

                            LocRunway &rw = loc.runways[loc.runway_count++];
                            rw.le_lat = le_lat;
                            rw.le_lon = le_lon;
                            rw.he_lat = he_lat;
                            rw.he_lon = he_lon;
                            strlcpy(rw.le_id, r["le_ident"] | "", sizeof(rw.le_id));
                            strlcpy(rw.he_id, r["he_ident"] | "", sizeof(rw.he_id));
                            Serial.printf("[locations]   %s/%s: closed=\"%s\" (parsed %d) -- KEPT (slot %d)\n", le_id_dbg, he_id_dbg, closed_raw, closed_parsed, loc.runway_count - 1);
                        }
                        Serial.printf("[locations] %s: final runway_count=%d\n", icao_upper, loc.runway_count);

                        _locations[_count++] = loc;
                        save_all();
                        ok = true;
                    } else {
                        fail("airport not found");
                    }
                } else {
                    fail("bad response from airportdb.io");
                }
            } else {
                fail("empty response");
            }
            heap_caps_free(buf);
        } else {
            fail("out of memory");
        }
    } else if (code == 404) {
        fail("airport not found");
    } else if (code == 401 || code == 403) {
        fail("invalid airportdb.io token");
    } else {
        char msg[32];
        snprintf(msg, sizeof(msg), "HTTP error %d", code);
        fail(msg);
    }

    http.end();
    http_mutex_release();
    return ok;
}

void locations_request_add(const char *icao) {
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    strlcpy(_add_pending_icao, icao, sizeof(_add_pending_icao));
    _add_pending = true;
    _add_result_ready = false;
    xSemaphoreGive(_add_mutex);
}

void locations_add_poll() {
    char icao[LOC_ICAO_LEN];
    bool has_request = false;

    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    if (_add_pending) {
        strlcpy(icao, _add_pending_icao, sizeof(icao));
        _add_pending = false;
        has_request = true;
    }
    xSemaphoreGive(_add_mutex);

    if (!has_request) return;

    char err[48] = {};
    bool ok = locations_add_from_icao(icao, err, sizeof(err));

    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    _add_result_ok = ok;
    strlcpy(_add_result_err, err, sizeof(_add_result_err));
    _add_result_ready = true;
    xSemaphoreGive(_add_mutex);
}

bool locations_add_result(bool *ok, char *err, size_t err_size) {
    bool ready;
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    ready = _add_result_ready;
    if (ready) {
        if (ok) *ok = _add_result_ok;
        if (err && err_size) strlcpy(err, _add_result_err, err_size);
        _add_result_ready = false;
    }
    xSemaphoreGive(_add_mutex);
    return ready;
}
