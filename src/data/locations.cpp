#include "locations.h"
#include "storage.h"
#include "http_mutex.h"
#include "fetcher.h"
#include "../ui/geo.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cctype>

// Static airport glyph DB (icao/lat/lon/large-flag only, no runway geometry
// -- see tools/generate_airports_db.py) is gitignored like static_map_data.h;
// same __has_include pattern map_view.cpp/radar_view.cpp use, needed here
// too since the nearby-large-airport scan (locations_nearby_set_enabled())
// reads it directly rather than going through a view file.
#if __has_include("../ui/airports_db.h")
#include "../ui/airports_db.h"
#define HAS_AIRPORTS_DB 1
#else
#define HAS_AIRPORTS_DB 0
#endif

// NetworkClientSecure's default TLS handshake timeout is 120s and is NOT
// bounded by HTTPClient::setTimeout() (that only covers the read phase after
// a connection succeeds) -- a slow/hung handshake here would otherwise hold
// http_mutex for up to two full minutes, starving every other network
// consumer in the app. Must construct the WiFiClientSecure ourselves and
// call setHandshakeTimeout() on it before HTTPClient::begin(), since the
// single-string begin(url) overload creates its own client with the 120s
// default baked in.
#define TLS_HANDSHAKE_TIMEOUT_S 8

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
static int _active_index = -1; // -1 = nothing selected (empty list, or none chosen yet)

// Nearby-large-airport cache -- only the *active* location's data is ever
// resident in DRAM (loaded lazily, see locations_nearby_get_active()).
// Everything else lives in NVS under a per-owner key until it's needed.
static Location _nearby[NEARBY_MAX];
static int _nearby_loaded_count = 0;
static int _nearby_loaded_for = -2; // sentinel: -2 = nothing loaded yet, else a _locations[] index

// Fetch queue for the currently-in-progress nearby-airport cache pass (one
// owner at a time, same "no dedicated task, ride location_poll_task's
// existing stack" reasoning as the add-by-ICAO queue below).
//
// The owner is tracked by NAME, NOT by _locations[] index -- a fetch pass
// can take several seconds to drain, during which locations_reorder()/
// locations_remove() can freely change what a given index points at (both
// are explicitly documented as shifting the array). Name is stable across
// both (only position changes, never identity). Resolving back to a current
// index at commit time (nearby_resolve_owner_idx()) avoids silently writing
// a completed fetch into the wrong location's cache slot.
static char _nearby_queue_icao[NEARBY_MAX][LOC_ICAO_LEN];
static int _nearby_queue_len = 0;
static int _nearby_queue_pos = 0;
static bool _nearby_queue_active = false;
static char _nearby_queue_owner_name[LOC_NAME_LEN] = {};
static Location _nearby_fetch_buf[NEARBY_MAX]; // accumulates results until the queue drains, then one NVS write
static int _nearby_fetch_buf_count = 0;

// If a toggle-on happens while a different owner's fetch pass is already in
// flight, it's queued here rather than clobbering the in-progress one (which
// would silently strand the first owner's toggle as "enabled" with nothing
// ever fetched for it, since a scan is only ever triggered on the off->on
// transition). Small fixed cap -- toggling more than a few locations on in
// one sitting before the first pass finishes is not a realistic case worth
// unbounded queuing for.
#define NEARBY_PENDING_OWNERS_MAX 4
static char _nearby_pending_name[NEARBY_PENDING_OWNERS_MAX][LOC_NAME_LEN];
static int _nearby_pending_count = 0;

// "Add by ICAO" request/response — processed by locations_add_poll(), called
// from location_poll_task's existing loop rather than a dedicated task.
static SemaphoreHandle_t _add_mutex = nullptr;
static bool _add_pending = false;
static char _add_pending_icao[LOC_ICAO_LEN] = {};
static bool _add_result_ready = false;
static bool _add_result_ok = false;
static char _add_result_err[48] = {};

// "Verify token" request/response -- same shape/reason as "add by ICAO"
// above, reusing _add_mutex rather than a second semaphore for one more
// small flag. See locations_verify_token_poll() for the actual check.
static bool _verify_pending = false;
static bool _verify_result_ready = false;
static bool _verify_result_ok = false;
static char _verify_result_err[48] = {};

// On-disk format: for each saved location, a fixed-size header (icao, name,
// lat, lon, elevation_ft, runway_count) followed by exactly runway_count
// LocRunway entries -- NOT a fixed MAX_RUNWAYS reservation. A location's
// runways[] array in memory is sized for the worst case (KORD-sized
// airports), but writing that full reservation to NVS for every saved
// airport regardless of how many runways it actually has is exactly the
// kind of large blocking flash write that visibly stalls this board's LCD
// panel (see project_p4_heap_constraints memory -- the cyan-flash bug).
// Packing tightly keeps the write proportional to real data.
// Also reused as-is for each entry in a nearby-airport cache blob (see
// nearby_commit()/locations_nearby_get_active()) -- same per-airport fields,
// just written under a different key and not counted against MAX_LOCATIONS.
// Growing this header changes the on-disk format -- same accepted precedent
// as the MAX_RUNWAYS bump and the nearby_enabled/nearby_count addition:
// saved locations reset to empty once on the first boot after upgrading.
struct LocationHeader {
    char icao[LOC_ICAO_LEN];
    char name[LOC_NAME_LEN];
    float lat, lon;
    int elevation_ft;
    int runway_count;
    int nearby_enabled; // stored as int, not bool, to keep the struct's
                          // layout unambiguous across a raw memcpy into NVS
    int nearby_count;
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
                strlcpy(hdr.name, loc.name, sizeof(hdr.name));
                hdr.lat = loc.lat;
                hdr.lon = loc.lon;
                hdr.elevation_ft = loc.elevation_ft;
                hdr.runway_count = loc.runway_count;
                hdr.nearby_enabled = loc.nearby_enabled ? 1 : 0;
                hdr.nearby_count = loc.nearby_count;
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
    int parsed = 0;
    if (_count > 0 && blob_len > 0) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(blob_len, MALLOC_CAP_SPIRAM);
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
                strlcpy(loc.name, hdr.name, sizeof(loc.name));
                loc.lat = hdr.lat;
                loc.lon = hdr.lon;
                loc.elevation_ft = hdr.elevation_ft;
                loc.runway_count = hdr.runway_count;
                loc.nearby_enabled = hdr.nearby_enabled != 0;
                loc.nearby_count = hdr.nearby_count;
                memcpy(loc.runways, buf + pos, rwy_bytes);
                pos += rwy_bytes;
            }
            heap_caps_free(buf);
        }
        if (parsed != _count) {
            // Inconsistent/corrupt/old-format blob -- don't trust partial data.
            _count = 0;
            memset(_locations, 0, sizeof(_locations));
        }
    } else if (_count > 0) {
        // count > 0 but no blob at all -- inconsistent, reset.
        _count = 0;
    }

    _prefs.end();

    // Resume-on-boot: restore whichever location was active last, matched by
    // NAME rather than raw index (a location's position in _locations[] can
    // shift across reboots -- removals compact the array) and rather than
    // ICAO (waypoints don't have one). Falls back to "nothing selected" (-1)
    // if it wasn't found (removed, or never set) -- there's no longer a
    // guaranteed fallback location the way Home used to be.
    _active_index = -1;
    if (g_config.last_location_name[0]) {
        for (int i = 0; i < _count; i++) {
            if (strcmp(_locations[i].name, g_config.last_location_name) == 0) {
                _active_index = i;
                break;
            }
        }
    }

    _add_mutex = xSemaphoreCreateMutex();
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
    bool was_active = (_active_index == idx);
    for (int i = idx; i < _count - 1; i++) {
        _locations[i] = _locations[i + 1];
    }
    _count--;
    memset(&_locations[_count], 0, sizeof(Location));
    if (_active_index == idx) _active_index = -1;
    else if (_active_index > idx) _active_index--;
    if (was_active && g_config.last_location_name[0]) {
        // Don't leave a stale name in NVS pointing at a now-removed location
        // -- resuming would fall back to "nothing selected" anyway (not
        // found in locations_init()'s lookup), but clear it here for
        // consistency.
        g_config.last_location_name[0] = '\0';
        storage_save_config(g_config);
    }
    save_all();
}

void locations_reorder(int from, int to) {
    if (from < 0 || from >= _count || to < 0 || to >= _count || from == to) return;

    Location moved = _locations[from];
    if (from < to) {
        for (int i = from; i < to; i++) _locations[i] = _locations[i + 1];
    } else {
        for (int i = from; i > to; i--) _locations[i] = _locations[i - 1];
    }
    _locations[to] = moved;

    // The active selection (if it was one of the affected slots) needs to
    // keep pointing at the same location it did before the shift, not the
    // same array index -- same reasoning as locations_remove()'s own index
    // bookkeeping below.
    if (_active_index == from) {
        _active_index = to;
    } else if (from < to && _active_index > from && _active_index <= to) {
        _active_index--;
    } else if (from > to && _active_index >= to && _active_index < from) {
        _active_index++;
    }

    save_all();
}

int locations_active_index() {
    return _active_index;
}

void locations_set_active(int idx) {
    if (idx < -1 || idx >= _count) return;
    bool changed = (idx != _active_index);
    _active_index = idx;

    // Persist for resume-on-boot -- this is only ever called from the
    // location picker's own row-tap handler, a discrete human action, so an
    // immediate NVS write is safe.
    const char *name = (idx == -1) ? "" : _locations[idx].name;
    if (strcmp(g_config.last_location_name, name) != 0) {
        strlcpy(g_config.last_location_name, name, sizeof(g_config.last_location_name));
        storage_save_config(g_config);
    }

    // Wake the fetch loop immediately rather than leaving the view showing
    // stale (or no) data until its next ~20s cadence tick -- this is the one
    // fetch path now (see fetcher.cpp), so switching location always means
    // "go get fresh data for this one now."
    if (changed) fetcher_request_immediate_fetch();
}

bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft) {
    const Location *loc = locations_get(_active_index);
    if (!loc) return false;
    if (lat) *lat = loc->lat;
    if (lon) *lon = loc->lon;
    if (elevation_ft) *elevation_ft = loc->elevation_ft;
    return true;
}

// NOTE: airportdb.io wraps OurAirports data. Field names below follow
// OurAirports' well-known airports.csv/runways.csv column names
// (latitude_deg/longitude_deg/elevation_ft, le_ident/le_latitude_deg/...).
// Not verified against a live response in this environment (no network
// access while writing this) — if fields come back zeroed/missing on real
// hardware, check the actual response shape and adjust the keys below.
//
// Shared by locations_add_from_icao() (adds a user-saved location) and the
// nearby-large-airport cache fetch (locations_nearby_poll()) -- same
// network/parsing logic, only what the caller does with the result differs.
// Does NOT check g_config.airportdb_token, MAX_LOCATIONS, or de-dupe against
// existing saves -- those are caller-specific and stay in
// locations_add_from_icao(); the nearby-cache path has none of them (it
// doesn't count against MAX_LOCATIONS and duplicates across different
// owners' nearby lists are fine).
static bool fetch_airport_data(const char *icao_upper, Location &out, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) return fail("network busy, try again");

    char url[160];
    snprintf(url, sizeof(url), "https://airportdb.io/api/v1/airport/%s?apiToken=%s",
             icao_upper, g_config.airportdb_token);

    WiFiClientSecure client;
    client.setInsecure(); // matches http.begin(url)'s own no-CA-cert behavior
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
    HTTPClient http;
    http.begin(client, url);
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
                        out = Location{};
                        strlcpy(out.icao, icao_upper, sizeof(out.icao));
                        strlcpy(out.name, icao_upper, sizeof(out.name)); // airports auto-name themselves after their ICAO
                        out.lat = lat;
                        out.lon = lon;
                        out.elevation_ft = doc["elevation_ft"].as<int>();

                        JsonArray rwys = doc["runways"].as<JsonArray>();
                        for (JsonObject r : rwys) {
                            if (out.runway_count >= MAX_RUNWAYS) break;
                            // Skip decommissioned runways -- OurAirports (which
                            // airportdb.io wraps) tracks this per-runway (e.g.
                            // KORD's old diagonals 14L/32R and 18/36 are marked
                            // closed=1 despite still having valid coordinates)
                            // and without this check we'd draw them as if
                            // active, and -- since MAX_RUNWAYS is a fixed cap --
                            // potentially crowd out a real active runway that
                            // arrives later in the array. Confirmed via a live
                            // airportdb.io response (2026-07) that this field
                            // and its string "0"/"1" encoding are handled
                            // correctly; airportdb.io's own data can still lag
                            // OurAirports for very recent runway changes (see
                            // project_location_architecture memory) -- that's
                            // a data-source staleness limitation, not a parsing
                            // bug, and isn't fixable here.
                            if (r["closed"].as<int>() == 1) continue;
                            float le_lat = r["le_latitude_deg"].as<float>();
                            float le_lon = r["le_longitude_deg"].as<float>();
                            float he_lat = r["he_latitude_deg"].as<float>();
                            float he_lon = r["he_longitude_deg"].as<float>();
                            // Skip runways OurAirports has no threshold coordinates for —
                            // draw only what we can actually place on the map.
                            if ((le_lat == 0.0f && le_lon == 0.0f) ||
                                (he_lat == 0.0f && he_lon == 0.0f)) continue;

                            LocRunway &rw = out.runways[out.runway_count++];
                            rw.le_lat = le_lat;
                            rw.le_lon = le_lon;
                            rw.he_lat = he_lat;
                            rw.he_lon = he_lon;
                            strlcpy(rw.le_id, r["le_ident"] | "", sizeof(rw.le_id));
                            strlcpy(rw.he_id, r["he_ident"] | "", sizeof(rw.he_id));
                        }

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
    } else if (code == 400 || code == 401 || code == 403) {
        // Confirmed on real hardware with a genuinely bad token: airportdb.io
        // returns 400, not 401/403 as originally assumed -- all three folded
        // into the same "invalid token" message rather than 400 falling
        // through to the generic "HTTP error 400" below, which gave no hint
        // at the actual cause.
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

bool locations_add_from_icao(const char *icao, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!icao || !icao[0]) return fail("no ICAO given");
    if (_count >= MAX_LOCATIONS) return fail("location list full");

    char icao_upper[LOC_ICAO_LEN] = {};
    strlcpy(icao_upper, icao, sizeof(icao_upper));
    for (char *p = icao_upper; *p; p++) *p = toupper((unsigned char)*p);

    // Check for an existing entry first — avoid duplicate network calls.
    for (int i = 0; i < _count; i++) {
        if (strcmp(_locations[i].icao, icao_upper) == 0) return fail("already saved");
    }

    Location loc;
    if (!g_config.airportdb_enabled || !g_config.airportdb_token[0]) {
        // No token / disabled -- can't fetch runway geometry, but that shouldn't
        // block adding the airport outright (runway_count == 0 is already a
        // normal state elsewhere: a plain waypoint, or a fetch that hasn't
        // completed yet). Fall back to the static large/medium glyph DB
        // (tools/generate_airports_db.py, compiled in independently of any
        // token) for coordinates -- it's the same DB the nearby-airports
        // scan above already reads, just no runway data in it by design.
#if HAS_AIRPORTS_DB
        bool found = false;
        for (int i = 0; i < AIRPORTS_DB_COUNT; i++) {
            if (strcmp(airports_db[i].icao, icao_upper) == 0) {
                loc = Location{};
                strlcpy(loc.icao, icao_upper, sizeof(loc.icao));
                strlcpy(loc.name, icao_upper, sizeof(loc.name));
                loc.lat = airports_db[i].lat;
                loc.lon = airports_db[i].lon;
                found = true;
                break;
            }
        }
        if (!found) {
            if (!g_config.airportdb_token[0])
                return fail("no airportdb.io token set, and this airport isn't in the static database");
            return fail("airportdb.io disabled, and this airport isn't in the static database");
        }
#else
        if (!g_config.airportdb_token[0]) return fail("no airportdb.io token set");
        return fail("airportdb.io disabled");
#endif
    } else if (!fetch_airport_data(icao_upper, loc, err, err_size)) {
        return false;
    }

    _locations[_count++] = loc;
    save_all();
    return true;
}

bool locations_add_waypoint(const char *name, float lat, float lon, int elevation_ft,
                             char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!name || !name[0]) return fail("no name given");
    if (_count >= MAX_LOCATIONS) return fail("location list full");

    // '|' is reserved as the serial-config protocol's field delimiter (see
    // serial_config.cpp) -- strip it defensively rather than reject the
    // whole name, same spirit as handle_line()'s own CR/LF/space trimming.
    char clean_name[LOC_NAME_LEN] = {};
    size_t j = 0;
    for (const char *p = name; *p && j < sizeof(clean_name) - 1; p++) {
        if (*p == '|') continue;
        clean_name[j++] = *p;
    }
    clean_name[j] = '\0';
    if (!clean_name[0]) return fail("name is empty");

    for (int i = 0; i < _count; i++) {
        if (strcmp(_locations[i].name, clean_name) == 0) return fail("name already used");
    }

    Location loc = {};
    strlcpy(loc.name, clean_name, sizeof(loc.name));
    // loc.icao stays all-zero -- empty ICAO is what marks this a waypoint
    // rather than an airport (see the Location struct comment in locations.h).
    loc.lat = lat;
    loc.lon = lon;
    loc.elevation_ft = elevation_ft;

    _locations[_count++] = loc;
    save_all();
    return true;
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

// Well-known, permanent airportdb.io entry used purely to test whether the
// currently-saved token authenticates -- never actually saved, the fetched
// Location is discarded. Real airports get renamed to their own ICAO on
// save (see fetch_airport_data() above), so reusing that same function here
// costs nothing extra beyond the one HTTPS round trip itself.
#define TOKEN_VERIFY_TEST_ICAO "KJFK"

void locations_request_verify_token() {
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    _verify_pending = true;
    _verify_result_ready = false;
    xSemaphoreGive(_add_mutex);
}

void locations_verify_token_poll() {
    bool has_request = false;
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    if (_verify_pending) {
        _verify_pending = false;
        has_request = true;
    }
    xSemaphoreGive(_add_mutex);

    if (!has_request) return;

    char err[48] = {};
    bool ok;
    if (!g_config.airportdb_token[0]) {
        ok = false;
        strlcpy(err, "no token set", sizeof(err));
    } else {
        Location discard;
        ok = fetch_airport_data(TOKEN_VERIFY_TEST_ICAO, discard, err, sizeof(err));
    }

    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    _verify_result_ok = ok;
    strlcpy(_verify_result_err, err, sizeof(_verify_result_err));
    _verify_result_ready = true;
    xSemaphoreGive(_add_mutex);
}

bool locations_verify_token_result(bool *ok, char *err, size_t err_size) {
    bool ready;
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    ready = _verify_result_ready;
    if (ready) {
        if (ok) *ok = _verify_result_ok;
        if (err && err_size) strlcpy(err, _verify_result_err, err_size);
        _verify_result_ready = false;
    }
    xSemaphoreGive(_add_mutex);
    return ready;
}

// ---- Nearby-large-airport cache ----------------------------------------
//
// No new synchronization here beyond what _locations[]/_count already have
// (none) -- locations_nearby_poll() runs on location_poll_task same as
// locations_add_poll(), and the picker/draw-side reads happen from the
// UI/render thread with the same accepted eventual-consistency as every
// other read of _locations[] in this file.

// FNV-1a 32-bit -- simple, deterministic, good enough collision resistance
// for at most MAX_LOCATIONS (15) items. Used to build a short, stable NVS
// key from a location's name (see nearby_nvs_key()) -- names can be up to
// LOC_NAME_LEN-1 (16) chars, well over Preferences' 15-char key limit, so
// the name itself can't be used as the key directly the way ICAO (<=7
// chars) used to be.
static uint32_t fnv1a_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

// "nb_" + 8 hex chars = 11 chars, comfortably under Preferences' 15-char
// key limit regardless of how long the location's actual name is.
static void nearby_nvs_key(int idx, char *out, size_t out_size) {
    const Location *loc = locations_get(idx);
    snprintf(out, out_size, "nb_%08lx", (unsigned long)fnv1a_hash(loc ? loc->name : ""));
}

bool locations_nearby_enabled(int idx) {
    const Location *loc = locations_get(idx);
    return loc ? loc->nearby_enabled : false;
}

int locations_nearby_count(int idx) {
    const Location *loc = locations_get(idx);
    return loc ? loc->nearby_count : 0;
}

// Writes the fully-drained fetch batch for `owner_idx` to NVS in one shot
// (not incrementally per-fetch -- avoids repeated read-modify-write churn on
// a blob that's growing across a burst of several sequential fetches), and
// updates that owner's persisted nearby_count header field. If the owner is
// the currently active location, also refreshes the resident draw-time
// cache (_nearby[]) immediately rather than waiting for the next active-
// index change to trigger a reload.
static void nearby_commit(int owner_idx, const Location *entries, int n) {
    char key[16];
    nearby_nvs_key(owner_idx, key, sizeof(key));

    _prefs.begin("adsb_locs", false);
    if (n > 0) {
        size_t buf_size = sizeof(int32_t);
        for (int i = 0; i < n; i++)
            buf_size += sizeof(LocationHeader) + (size_t)entries[i].runway_count * sizeof(LocRunway);

        uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t pos = 0;
            int32_t cnt = n;
            memcpy(buf, &cnt, sizeof(cnt));
            pos += sizeof(cnt);
            for (int i = 0; i < n; i++) {
                const Location &e = entries[i];
                LocationHeader hdr;
                strlcpy(hdr.icao, e.icao, sizeof(hdr.icao));
                strlcpy(hdr.name, e.name, sizeof(hdr.name));
                hdr.lat = e.lat;
                hdr.lon = e.lon;
                hdr.elevation_ft = e.elevation_ft;
                hdr.runway_count = e.runway_count;
                hdr.nearby_enabled = 0; // unused for nearby-cache entries themselves
                hdr.nearby_count = 0;
                memcpy(buf + pos, &hdr, sizeof(hdr));
                pos += sizeof(hdr);
                size_t rwy_bytes = (size_t)e.runway_count * sizeof(LocRunway);
                memcpy(buf + pos, e.runways, rwy_bytes);
                pos += rwy_bytes;
            }
            _prefs.putBytes(key, buf, buf_size);
            heap_caps_free(buf);
        }
    } else if (_prefs.isKey(key)) {
        // isKey() first -- the common "nothing nearby" case (e.g. KDEN, no
        // other large airport within the widest radius preset) hits this
        // branch on the very first scan, before any blob was ever written
        // for this key. remove()'s underlying nvs_erase_key() logs a scary
        // "NOT_FOUND" [E] line for that case even though it is harmless
        // (reported) -- nothing to erase isn't a real error here.
        _prefs.remove(key);
    }
    _prefs.end();

    if (owner_idx >= 0 && owner_idx < _count) {
        _locations[owner_idx].nearby_count = n;
        save_all();
    }

    if (owner_idx == _active_index) {
        int copy_n = (n < NEARBY_MAX) ? n : NEARBY_MAX;
        memcpy(_nearby, entries, copy_n * sizeof(Location));
        _nearby_loaded_count = copy_n;
        _nearby_loaded_for = owner_idx;
    }
}

// Resolves the in-flight queue's owner (tracked by name, see above) back to
// a current _locations[] index at commit time. Returns -2 if the owner was
// removed while its fetch pass was still in flight (nothing to commit it
// into anymore).
static int nearby_resolve_owner_idx() {
    for (int i = 0; i < _count; i++)
        if (strcmp(_locations[i].name, _nearby_queue_owner_name) == 0) return i;
    return -2;
}

// Builds and starts the static-DB scan queue for one owner. Assumes no
// other pass is currently active -- callers check _nearby_queue_active
// first (locations_nearby_set_enabled queues instead of calling this
// directly when one is already running).
static void nearby_start_scan(int idx) {
#if HAS_AIRPORTS_DB
    const Location *owner = locations_get(idx);
    if (!owner) return;

    // Widest configured radius preset -- same "fetch once, cover every zoom
    // level" reasoning as the primary location save itself, so this never
    // needs to re-fetch as the user zooms in/out afterward.
    float radius = (float)g_config.radius_presets[3];

    _nearby_queue_len = 0;
    for (int i = 0; i < AIRPORTS_DB_COUNT && _nearby_queue_len < NEARBY_MAX; i++) {
        const StaticAirport &ap = airports_db[i];
        if (!ap.large) continue; // large airports only -- see locations.h
        // Skip the owner itself -- only ever matches for an airport-type
        // owner (owner->icao is empty for a waypoint, so this never
        // falsely excludes anything for that case).
        if (owner->icao[0] && strcmp(ap.icao, owner->icao) == 0) continue;
        if (MapProjection::distance_nm(owner->lat, owner->lon, ap.lat, ap.lon) > radius) continue;
        strlcpy(_nearby_queue_icao[_nearby_queue_len], ap.icao, LOC_ICAO_LEN);
        _nearby_queue_len++;
    }
    _nearby_queue_pos = 0;
    strlcpy(_nearby_queue_owner_name, owner->name, sizeof(_nearby_queue_owner_name));
    _nearby_fetch_buf_count = 0;
    _nearby_queue_active = true;

    if (_nearby_queue_len == 0) {
        // Nothing nearby -- persist the (empty) result immediately so this
        // isn't re-scanned every time the toggle is flipped.
        _nearby_queue_active = false;
        nearby_commit(idx, nullptr, 0);
    }
#endif
}

static void nearby_queue_pending(int idx) {
    if (_nearby_pending_count >= NEARBY_PENDING_OWNERS_MAX) return; // best-effort cap, silently dropped
    const Location *loc = locations_get(idx);
    if (!loc) return;
    strlcpy(_nearby_pending_name[_nearby_pending_count++], loc->name, LOC_NAME_LEN);
}

void locations_nearby_set_enabled(int idx, bool on) {
    if (idx < 0 || idx >= _count) return;
    if (_locations[idx].nearby_enabled == on) return;
    _locations[idx].nearby_enabled = on;
    save_all();

    // The resident draw-time cache (_nearby[]) only reloads when the
    // *active index* changes (see locations_nearby_get_active()) -- it
    // never re-checks the enabled flag on its own while staying on the
    // same location. Invalidate it here on every flip, not just off, so the
    // next draw call re-evaluates from scratch regardless of direction:
    // off needs it to stop returning the already-loaded runways (reported:
    // had no visible effect); on needs it to start returning the
    // already-on-disk data again instead of the "0, already evaluated"
    // result the *previous* off toggle left cached, which otherwise left
    // turning it back on invisible until switching away and back to this
    // location forced a real reload (reported, same underlying cause).
    if (idx == _active_index) _nearby_loaded_for = -2;

    if (!on) return;
    if (locations_nearby_count(idx) > 0) return; // already cached, nothing to (re)fetch

    if (_nearby_queue_active) {
        nearby_queue_pending(idx); // another owner's pass is already running -- wait our turn
    } else {
        nearby_start_scan(idx);
    }
}

void locations_nearby_poll() {
    if (!_nearby_queue_active || _nearby_queue_pos >= _nearby_queue_len) return;

    const char *icao = _nearby_queue_icao[_nearby_queue_pos++];
    if (_nearby_fetch_buf_count < NEARBY_MAX) {
        Location entry;
        char err[48];
        // Best-effort: one bad/rate-limited fetch shouldn't abort caching
        // the rest of the queue -- this is a background visual enhancement,
        // not a user-facing add flow with its own error surface to report to
        // (unlike locations_add_from_icao(), whose failure the picker shows
        // directly).
        if (fetch_airport_data(icao, entry, err, sizeof(err))) {
            _nearby_fetch_buf[_nearby_fetch_buf_count++] = entry;
        }
    }

    if (_nearby_queue_pos >= _nearby_queue_len) {
        _nearby_queue_active = false;
        int resolved = nearby_resolve_owner_idx();
        // resolved == -2: the owner was removed mid-fetch -- drop the
        // results, there's nothing left to commit them into.
        if (resolved != -2) nearby_commit(resolved, _nearby_fetch_buf, _nearby_fetch_buf_count);

        if (_nearby_pending_count > 0) {
            char next_name[LOC_NAME_LEN];
            strlcpy(next_name, _nearby_pending_name[0], sizeof(next_name));
            for (int i = 1; i < _nearby_pending_count; i++)
                strlcpy(_nearby_pending_name[i - 1], _nearby_pending_name[i], LOC_NAME_LEN);
            _nearby_pending_count--;

            int next_idx = -1;
            for (int i = 0; i < _count; i++)
                if (strcmp(_locations[i].name, next_name) == 0) { next_idx = i; break; }
            if (next_idx != -1) nearby_start_scan(next_idx); // else: that owner was removed while pending -- just drop it
        }
    }
}

const Location* locations_nearby_get_active(int *count) {
    if (_nearby_loaded_for != _active_index) {
        _nearby_loaded_count = 0;
        if (locations_nearby_enabled(_active_index)) {
            char key[16];
            nearby_nvs_key(_active_index, key, sizeof(key));
            _prefs.begin("adsb_locs", true);
            size_t blob_len = _prefs.getBytesLength(key);
            if (blob_len >= sizeof(int32_t)) {
                uint8_t *buf = (uint8_t *)heap_caps_malloc(blob_len, MALLOC_CAP_SPIRAM);
                if (buf) {
                    size_t got = _prefs.getBytes(key, buf, blob_len);
                    size_t pos = 0;
                    int32_t cnt = 0;
                    if (pos + sizeof(cnt) <= got) {
                        memcpy(&cnt, buf, sizeof(cnt));
                        pos += sizeof(cnt);
                    }
                    int parsed = 0;
                    for (; parsed < cnt && parsed < NEARBY_MAX; parsed++) {
                        if (pos + sizeof(LocationHeader) > got) break; // truncated -- bail
                        LocationHeader hdr;
                        memcpy(&hdr, buf + pos, sizeof(hdr));
                        pos += sizeof(hdr);
                        if (hdr.runway_count < 0 || hdr.runway_count > MAX_RUNWAYS) break; // corrupt
                        size_t rwy_bytes = (size_t)hdr.runway_count * sizeof(LocRunway);
                        if (pos + rwy_bytes > got) break; // truncated

                        Location &e = _nearby[parsed];
                        strlcpy(e.icao, hdr.icao, sizeof(e.icao));
                        strlcpy(e.name, hdr.name, sizeof(e.name));
                        e.lat = hdr.lat;
                        e.lon = hdr.lon;
                        e.elevation_ft = hdr.elevation_ft;
                        e.runway_count = hdr.runway_count;
                        e.nearby_enabled = false;
                        e.nearby_count = 0;
                        memcpy(e.runways, buf + pos, rwy_bytes);
                        pos += rwy_bytes;
                    }
                    _nearby_loaded_count = parsed;
                    heap_caps_free(buf);
                }
            }
            _prefs.end();
        }
        _nearby_loaded_for = _active_index;
    }
    if (count) *count = _nearby_loaded_count;
    return _nearby;
}

void locations_nearby_cache_clear() {
    _prefs.begin("adsb_locs", false);
    for (int i = 0; i < _count; i++) {
        char key[16];
        nearby_nvs_key(i, key, sizeof(key));
        if (_prefs.isKey(key)) _prefs.remove(key);
        _locations[i].nearby_count = 0;
    }
    _prefs.end();

    _nearby_loaded_count = 0;
    _nearby_loaded_for = -2;
    memset(_nearby, 0, sizeof(_nearby));
    // Drop any in-flight / pending nearby fetch -- results would just
    // repopulate what the user asked to clear.
    _nearby_queue_active = false;
    _nearby_queue_len = 0;
    _nearby_queue_pos = 0;
    _nearby_fetch_buf_count = 0;
    _nearby_pending_count = 0;

    save_all();
    Serial.println("Locations: nearby-airport caches cleared");
}

void locations_factory_reset() {
    _prefs.begin("adsb_locs", false);
    _prefs.clear(); // takes every nb_<hash> nearby-cache blob with it too, same namespace
    _prefs.end();
    Serial.println("Locations: adsb_locs namespace erased (factory reset)");
}
