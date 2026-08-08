// Linux implementation of src/data/locations.h -- fresh implementation, not
// a port of src/data/locations.cpp. That file's on-disk format is a packed
// binary blob tuned specifically for NVS's flash-write-size constraints
// (see its own LocationHeader comment) and its add/nearby-scan flow is built
// around ESP32's "no spare task stack" constraint (poll functions driven by
// fetcher.cpp's location_poll_task, see project_p4_heap_constraints memory)
// -- neither concern applies on Linux. Same reasoning as
// pi/platform_linux/datasource_remote.cpp: reimplement the same interface
// fresh rather than force a shared abstraction onto genuinely
// platform-specific plumbing (see project_pi_port memory).
//
// Storage: a single JSON file (~/.config/adsb/locations.json), one entry per
// saved location, each with its own "nearby" array embedded directly.
// Unlike the ESP32 side (which lazily loads only the *active* location's
// nearby-airport cache to save DRAM), every location's nearby list is kept
// resident here -- MAX_LOCATIONS(15) x NEARBY_MAX(20) Locations is trivial
// on a Pi 3B's 1GB RAM, and it deletes a whole lazy-load/invalidation dance
// (_nearby_loaded_for tracking) for no real benefit on this hardware.
//
// Concurrency: add-by-ICAO and the nearby-airport scan both do blocking
// HTTPS round trips (via platform_http_get), so both run on detached
// std::thread's rather than ESP32's poll-from-an-existing-task-loop
// approach -- Linux threads are cheap and there's no shared-stack budget to
// protect. locations_add_poll()/locations_nearby_poll() are kept as
// link-compatible no-ops (location_picker.cpp still calls the former from
// its own timer) since the threads commit results directly.

#include "../../src/data/locations.h"
#include "../../src/data/storage.h"
#include "../../src/data/fetcher.h"
#include "../../src/ui/geo.h"
#include "../../src/platform/platform.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <sys/stat.h>

#if __has_include("../../src/ui/airports_db.h")
#include "../../src/ui/airports_db.h"
#define HAS_AIRPORTS_DB 1
#else
#define HAS_AIRPORTS_DB 0
#endif

namespace {

std::mutex _mutex; // guards _locations/_count/_active_index/_nearby_all(_count)
Location _locations[MAX_LOCATIONS];
int _count = 0;
int _active_index = -1;

Location _nearby_all[MAX_LOCATIONS][NEARBY_MAX];
int _nearby_all_count[MAX_LOCATIONS] = {};

std::mutex _add_mutex;
bool _add_result_ready = false;
bool _add_result_ok = false;
char _add_result_err[48] = {};

std::mutex _verify_mutex;
bool _verify_result_ready = false;
bool _verify_result_ok = false;
char _verify_result_err[48] = {};

bool _nearby_scan_active = false; // best-effort: skip a second concurrent scan request

std::string config_dir() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return std::string(xdg) + "/adsb";
    const char *home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/adsb";
}

std::string locations_file_path() {
    return config_dir() + "/locations.json";
}

void loc_to_json(JsonObject obj, const Location &loc) {
    obj["icao"] = loc.icao;
    obj["name"] = loc.name;
    obj["lat"] = loc.lat;
    obj["lon"] = loc.lon;
    obj["elev"] = loc.elevation_ft;
    JsonArray rwys = obj["runways"].to<JsonArray>();
    for (int i = 0; i < loc.runway_count; i++) {
        JsonObject r = rwys.add<JsonObject>();
        r["le_lat"] = loc.runways[i].le_lat;
        r["le_lon"] = loc.runways[i].le_lon;
        r["he_lat"] = loc.runways[i].he_lat;
        r["he_lon"] = loc.runways[i].he_lon;
        r["le_id"] = loc.runways[i].le_id;
        r["he_id"] = loc.runways[i].he_id;
    }
}

void json_to_loc(JsonObjectConst obj, Location &loc) {
    loc = Location{};
    strlcpy(loc.icao, obj["icao"] | "", sizeof(loc.icao));
    strlcpy(loc.name, obj["name"] | "", sizeof(loc.name));
    loc.lat = obj["lat"] | 0.0f;
    loc.lon = obj["lon"] | 0.0f;
    loc.elevation_ft = obj["elev"] | 0;
    JsonArrayConst rwys = obj["runways"];
    for (JsonObjectConst r : rwys) {
        if (loc.runway_count >= MAX_RUNWAYS) break;
        LocRunway &rw = loc.runways[loc.runway_count++];
        rw.le_lat = r["le_lat"] | 0.0f;
        rw.le_lon = r["le_lon"] | 0.0f;
        rw.he_lat = r["he_lat"] | 0.0f;
        rw.he_lon = r["he_lon"] | 0.0f;
        strlcpy(rw.le_id, r["le_id"] | "", sizeof(rw.le_id));
        strlcpy(rw.he_id, r["he_id"] | "", sizeof(rw.he_id));
    }
}

// Caller must hold _mutex.
void save_all_locked() {
    mkdir(config_dir().c_str(), 0755);

    JsonDocument doc;
    JsonArray arr = doc["locations"].to<JsonArray>();
    for (int i = 0; i < _count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        loc_to_json(obj, _locations[i]);
        obj["nearby_on"] = _locations[i].nearby_enabled;
        JsonArray nb = obj["nearby"].to<JsonArray>();
        for (int j = 0; j < _nearby_all_count[i]; j++) {
            JsonObject nobj = nb.add<JsonObject>();
            loc_to_json(nobj, _nearby_all[i][j]);
        }
    }

    FILE *f = fopen(locations_file_path().c_str(), "w");
    if (!f) {
        platform_log("Locations: failed to open %s for writing\n", locations_file_path().c_str());
        return;
    }
    std::string out;
    serializeJson(doc, out);
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

void load_all_locked() {
    _count = 0;
    memset(_locations, 0, sizeof(_locations));
    memset(_nearby_all_count, 0, sizeof(_nearby_all_count));

    FILE *f = fopen(locations_file_path().c_str(), "r");
    if (!f) {
        platform_log("Locations: no saved-locations file yet at %s\n", locations_file_path().c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }
    std::string buf(size, '\0');
    size_t got = fread(&buf[0], 1, size, f);
    fclose(f);
    buf.resize(got);

    JsonDocument doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) {
        platform_log("Locations: %s failed to parse\n", locations_file_path().c_str());
        return;
    }

    JsonArrayConst arr = doc["locations"];
    for (JsonObjectConst obj : arr) {
        if (_count >= MAX_LOCATIONS) break;
        Location &loc = _locations[_count];
        json_to_loc(obj, loc);
        loc.nearby_enabled = obj["nearby_on"] | false;
        JsonArrayConst nb = obj["nearby"];
        int n = 0;
        for (JsonObjectConst nobj : nb) {
            if (n >= NEARBY_MAX) break;
            json_to_loc(nobj, _nearby_all[_count][n]);
            n++;
        }
        _nearby_all_count[_count] = n;
        loc.nearby_count = n;
        _count++;
    }

    platform_log("Locations: loaded %d saved location(s) from %s\n", _count, locations_file_path().c_str());
}

// Same OurAirports/airportdb.io field-name mapping as src/data/locations.cpp's
// fetch_airport_data() -- see that file's comment for why (airportdb.io
// wraps OurAirports' airports.csv/runways.csv column names verbatim, and
// coordinates come back as JSON strings, not numbers).
bool fetch_airport_data(const char *icao_upper, Location &out, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    char url[192];
    snprintf(url, sizeof(url), "https://airportdb.io/api/v1/airport/%s?apiToken=%s",
              icao_upper, g_config.airportdb_token);

    static thread_local std::vector<char> buf(256 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) {
        return fail("network request failed (bad token, no airport, or no network)");
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
        return fail("bad response from airportdb.io");
    }

    float lat = doc["latitude_deg"].as<float>();
    float lon = doc["longitude_deg"].as<float>();
    if (lat == 0.0f && lon == 0.0f) return fail("airport not found");

    out = Location{};
    strlcpy(out.icao, icao_upper, sizeof(out.icao));
    strlcpy(out.name, icao_upper, sizeof(out.name)); // airports auto-name themselves after their ICAO
    out.lat = lat;
    out.lon = lon;
    out.elevation_ft = doc["elevation_ft"].as<int>();

    JsonArrayConst rwys = doc["runways"];
    for (JsonObjectConst r : rwys) {
        if (out.runway_count >= MAX_RUNWAYS) break;
        if (r["closed"].as<int>() == 1) continue; // decommissioned -- see locations.cpp's own comment
        float le_lat = r["le_latitude_deg"].as<float>();
        float le_lon = r["le_longitude_deg"].as<float>();
        float he_lat = r["he_latitude_deg"].as<float>();
        float he_lon = r["he_longitude_deg"].as<float>();
        if ((le_lat == 0.0f && le_lon == 0.0f) || (he_lat == 0.0f && he_lon == 0.0f)) continue;

        LocRunway &rw = out.runways[out.runway_count++];
        rw.le_lat = le_lat;
        rw.le_lon = le_lon;
        rw.he_lat = he_lat;
        rw.he_lon = he_lon;
        strlcpy(rw.le_id, r["le_ident"] | "", sizeof(rw.le_id));
        strlcpy(rw.he_id, r["he_ident"] | "", sizeof(rw.he_id));
    }

    return true;
}

} // namespace

void locations_init() {
    std::lock_guard<std::mutex> lock(_mutex);
    load_all_locked();

    _active_index = -1;
    if (g_config.last_location_name[0]) {
        for (int i = 0; i < _count; i++) {
            if (strcmp(_locations[i].name, g_config.last_location_name) == 0) {
                _active_index = i;
                break;
            }
        }
    }
}

int locations_count() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _count;
}

// Returns a pointer into the internal array -- callers on Pi are all
// single-threaded UI-thread readers that use the result immediately (same
// eventual-consistency contract as the ESP32 side), so a snapshot copy isn't
// needed here.
const Location *locations_get(int idx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (idx < 0 || idx >= _count) return nullptr;
    return &_locations[idx];
}

void locations_remove(int idx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (idx < 0 || idx >= _count) return;
    for (int i = idx; i < _count - 1; i++) {
        _locations[i] = _locations[i + 1];
        memcpy(_nearby_all[i], _nearby_all[i + 1], sizeof(_nearby_all[i]));
        _nearby_all_count[i] = _nearby_all_count[i + 1];
    }
    _count--;
    memset(&_locations[_count], 0, sizeof(Location));
    _nearby_all_count[_count] = 0;

    if (_active_index == idx) {
        _active_index = -1;
        if (g_config.last_location_name[0]) {
            g_config.last_location_name[0] = '\0';
            storage_save_config(g_config);
        }
    } else if (_active_index > idx) {
        _active_index--;
    }
    save_all_locked();
}

void locations_reorder(int from, int to) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (from < 0 || from >= _count || to < 0 || to >= _count || from == to) return;

    Location moved = _locations[from];
    Location moved_nearby[NEARBY_MAX];
    memcpy(moved_nearby, _nearby_all[from], sizeof(moved_nearby));
    int moved_nearby_count = _nearby_all_count[from];

    if (from < to) {
        for (int i = from; i < to; i++) {
            _locations[i] = _locations[i + 1];
            memcpy(_nearby_all[i], _nearby_all[i + 1], sizeof(_nearby_all[i]));
            _nearby_all_count[i] = _nearby_all_count[i + 1];
        }
    } else {
        for (int i = from; i > to; i--) {
            _locations[i] = _locations[i - 1];
            memcpy(_nearby_all[i], _nearby_all[i - 1], sizeof(_nearby_all[i]));
            _nearby_all_count[i] = _nearby_all_count[i - 1];
        }
    }
    _locations[to] = moved;
    memcpy(_nearby_all[to], moved_nearby, sizeof(moved_nearby));
    _nearby_all_count[to] = moved_nearby_count;

    if (_active_index == from) {
        _active_index = to;
    } else if (from < to && _active_index > from && _active_index <= to) {
        _active_index--;
    } else if (from > to && _active_index >= to && _active_index < from) {
        _active_index++;
    }

    save_all_locked();
}

int locations_active_index() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _active_index;
}

void locations_set_active(int idx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (idx < -1 || idx >= _count) return;
    bool changed = (idx != _active_index);
    _active_index = idx;

    const char *name = (idx == -1) ? "" : _locations[idx].name;
    if (strcmp(g_config.last_location_name, name) != 0) {
        strlcpy(g_config.last_location_name, name, sizeof(g_config.last_location_name));
        storage_save_config(g_config);
    }

    if (changed) fetcher_request_immediate_fetch();
}

bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_active_index < 0 || _active_index >= _count) return false;
    const Location &loc = _locations[_active_index];
    if (lat) *lat = loc.lat;
    if (lon) *lon = loc.lon;
    if (elevation_ft) *elevation_ft = loc.elevation_ft;
    return true;
}

bool locations_add_from_icao(const char *icao, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!icao || !icao[0]) return fail("no ICAO given");

    char icao_upper[LOC_ICAO_LEN] = {};
    strlcpy(icao_upper, icao, sizeof(icao_upper));
    for (char *p = icao_upper; *p; p++) *p = toupper((unsigned char)*p);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_count >= MAX_LOCATIONS) return fail("location list full");
        for (int i = 0; i < _count; i++)
            if (strcmp(_locations[i].icao, icao_upper) == 0) return fail("already saved");
    }

    Location loc;
    if (!g_config.airportdb_enabled || !g_config.airportdb_token[0]) {
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

    std::lock_guard<std::mutex> lock(_mutex);
    if (_count >= MAX_LOCATIONS) return fail("location list full");
    for (int i = 0; i < _count; i++)
        if (strcmp(_locations[i].icao, icao_upper) == 0) return fail("already saved");
    _locations[_count++] = loc;
    save_all_locked();
    return true;
}

bool locations_add_waypoint(const char *name, float lat, float lon, int elevation_ft,
                             char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!name || !name[0]) return fail("no name given");

    char clean_name[LOC_NAME_LEN] = {};
    size_t j = 0;
    for (const char *p = name; *p && j < sizeof(clean_name) - 1; p++) {
        if (*p == '|') continue;
        clean_name[j++] = *p;
    }
    clean_name[j] = '\0';
    if (!clean_name[0]) return fail("name is empty");

    std::lock_guard<std::mutex> lock(_mutex);
    if (_count >= MAX_LOCATIONS) return fail("location list full");
    for (int i = 0; i < _count; i++)
        if (strcmp(_locations[i].name, clean_name) == 0) return fail("name already used");

    Location loc = {};
    strlcpy(loc.name, clean_name, sizeof(loc.name));
    loc.lat = lat;
    loc.lon = lon;
    loc.elevation_ft = elevation_ft;

    _locations[_count++] = loc;
    save_all_locked();
    return true;
}

// Async add: spawns a detached thread rather than using ESP32's
// request/poll-from-an-existing-task dance (see file header comment) --
// there's no shared-stack budget to protect on Linux.
void locations_request_add(const char *icao) {
    std::string icao_copy(icao ? icao : "");
    {
        std::lock_guard<std::mutex> lock(_add_mutex);
        _add_result_ready = false;
    }
    std::thread([icao_copy]() {
        char err[48] = {};
        bool ok = locations_add_from_icao(icao_copy.c_str(), err, sizeof(err));
        std::lock_guard<std::mutex> lock(_add_mutex);
        _add_result_ok = ok;
        strlcpy(_add_result_err, err, sizeof(_add_result_err));
        _add_result_ready = true;
    }).detach();
}

// No-op -- the thread spawned by locations_request_add() commits its result
// directly, there's nothing to drain from a poll loop. Kept only so
// location_picker.cpp (which calls this from its own timer, matching the
// ESP32 side) links unchanged.
void locations_add_poll() {}

bool locations_add_result(bool *ok, char *err, size_t err_size) {
    std::lock_guard<std::mutex> lock(_add_mutex);
    if (!_add_result_ready) return false;
    if (ok) *ok = _add_result_ok;
    if (err && err_size) strlcpy(err, _add_result_err, err_size);
    _add_result_ready = false;
    return true;
}

#define TOKEN_VERIFY_TEST_ICAO "KJFK"

void locations_request_verify_token() {
    {
        std::lock_guard<std::mutex> lock(_verify_mutex);
        _verify_result_ready = false;
    }
    std::thread([]() {
        char err[48] = {};
        bool ok;
        if (!g_config.airportdb_token[0]) {
            ok = false;
            strlcpy(err, "no token set", sizeof(err));
        } else {
            Location discard;
            ok = fetch_airport_data(TOKEN_VERIFY_TEST_ICAO, discard, err, sizeof(err));
        }
        std::lock_guard<std::mutex> lock(_verify_mutex);
        _verify_result_ok = ok;
        strlcpy(_verify_result_err, err, sizeof(_verify_result_err));
        _verify_result_ready = true;
    }).detach();
}

void locations_verify_token_poll() {}

bool locations_verify_token_result(bool *ok, char *err, size_t err_size) {
    std::lock_guard<std::mutex> lock(_verify_mutex);
    if (!_verify_result_ready) return false;
    if (ok) *ok = _verify_result_ok;
    if (err && err_size) strlcpy(err, _verify_result_err, err_size);
    _verify_result_ready = false;
    return true;
}

bool locations_nearby_enabled(int idx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (idx < 0 || idx >= _count) return false;
    return _locations[idx].nearby_enabled;
}

int locations_nearby_count(int idx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (idx < 0 || idx >= _count) return 0;
    return _locations[idx].nearby_count;
}

void locations_nearby_set_enabled(int idx, bool on) {
    std::string owner_name;
    float owner_lat = 0, owner_lon = 0;
    char owner_icao[LOC_ICAO_LEN] = {};
    bool need_scan = false;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (idx < 0 || idx >= _count) return;
        if (_locations[idx].nearby_enabled == on) return;
        _locations[idx].nearby_enabled = on;
        save_all_locked();

        if (on && _locations[idx].nearby_count == 0 && !_nearby_scan_active) {
            owner_name = _locations[idx].name;
            owner_lat = _locations[idx].lat;
            owner_lon = _locations[idx].lon;
            strlcpy(owner_icao, _locations[idx].icao, sizeof(owner_icao));
            _nearby_scan_active = true;
            need_scan = true;
        }
    }

    if (!need_scan) return;

#if HAS_AIRPORTS_DB
    float radius = (float)g_config.radius_presets[3];
    std::thread([owner_name, owner_lat, owner_lon, owner_icao_str = std::string(owner_icao), radius]() {
        std::vector<Location> found;
        for (int i = 0; i < AIRPORTS_DB_COUNT && (int)found.size() < NEARBY_MAX; i++) {
            const StaticAirport &ap = airports_db[i];
            if (!ap.large) continue;
            if (!owner_icao_str.empty() && owner_icao_str == ap.icao) continue;
            if (MapProjection::distance_nm(owner_lat, owner_lon, ap.lat, ap.lon) > radius) continue;

            Location entry;
            char err[48];
            if (fetch_airport_data(ap.icao, entry, err, sizeof(err))) {
                found.push_back(entry);
            }
        }

        std::lock_guard<std::mutex> lock(_mutex);
        int resolved = -1;
        for (int i = 0; i < _count; i++)
            if (strcmp(_locations[i].name, owner_name.c_str()) == 0) { resolved = i; break; }
        if (resolved != -1) {
            int n = (int)found.size();
            if (n > NEARBY_MAX) n = NEARBY_MAX;
            for (int i = 0; i < n; i++) _nearby_all[resolved][i] = found[i];
            _nearby_all_count[resolved] = n;
            _locations[resolved].nearby_count = n;
            save_all_locked();
        }
        _nearby_scan_active = false;
    }).detach();
#else
    std::lock_guard<std::mutex> lock(_mutex);
    _nearby_scan_active = false;
#endif
}

// No-op -- see locations_add_poll()'s comment; the nearby scan above runs to
// completion on its own thread rather than draining one queued fetch per
// poll call.
void locations_nearby_poll() {}

const Location *locations_nearby_get_active(int *count) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_active_index < 0 || _active_index >= _count) {
        if (count) *count = 0;
        return nullptr;
    }
    if (count) *count = _nearby_all_count[_active_index];
    return _nearby_all[_active_index];
}

void locations_nearby_cache_clear() {
    int restart_idx = -1;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (int i = 0; i < _count; i++) {
            memset(_nearby_all[i], 0, sizeof(_nearby_all[i]));
            _nearby_all_count[i] = 0;
            _locations[i].nearby_count = 0;
        }
        if (_count > 0) save_all_locked();

        // Re-kick a scan for the active location if its eye toggle is still
        // on -- otherwise the map stays empty until the user toggles it.
        if (_active_index >= 0 && _active_index < _count &&
            _locations[_active_index].nearby_enabled) {
            restart_idx = _active_index;
        }
        platform_log("Locations: nearby-airport caches cleared\n");
    }

    if (restart_idx < 0) return;
    // off→on reuses the existing fetch path (set_enabled no-ops if already on).
    locations_nearby_set_enabled(restart_idx, false);
    locations_nearby_set_enabled(restart_idx, true);
}

void locations_factory_reset() {
    std::lock_guard<std::mutex> lock(_mutex);
    _count = 0;
    _active_index = -1;
    memset(_locations, 0, sizeof(_locations));
    memset(_nearby_all_count, 0, sizeof(_nearby_all_count));
    remove(locations_file_path().c_str());
    platform_log("Locations: %s removed (factory reset)\n", locations_file_path().c_str());
}
