#include "fetcher.h"
#include "error_log.h"
#include "http_mutex.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include "../data/locations.h"
#include "../ui/alerts.h"
#if defined(USE_ETHERNET)
#include <ETH.h>
#endif
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// PSRAM allocator for ArduinoJson — keeps internal RAM free for SDIO/WiFi buffers
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

// Hard-reset the ESP32-C6 WiFi coprocessor via its reset pin.
// ESP.restart() only resets the P4 — the C6 retains its bad state.
static void reset_wifi_c6() {
    Serial.println("Resetting C6 WiFi module...");
    pinMode(WIFI_C6_RST, OUTPUT);
    digitalWrite(WIFI_C6_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(WIFI_C6_RST, HIGH);
    // Poll until ESP-Hosted SDIO link is up rather than using a fixed delay
    uint32_t t0 = millis();
    wl_status_t s;
    do {
        vTaskDelay(pdMS_TO_TICKS(200));
        s = WiFi.status();
    } while (s == WL_NO_SHIELD && millis() - t0 < 10000);
    vTaskDelay(pdMS_TO_TICKS(1000)); // let radio settle after SDIO link is up
    Serial.printf("C6 reset complete (%lums, status=%d)\n", millis() - t0, (int)s);
}

// Attempt WiFi connection with timeout. Returns true if connected.
static bool wifi_connect_with_timeout(uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.setScanMethod(WIFI_FAST_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    Serial.printf("WiFi connecting to '%s'...\n", g_config.wifi_ssid);
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_pass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            wl_status_t s = WiFi.status();
            const char *reason =
                s == WL_NO_SSID_AVAIL  ? "SSID not found" :
                s == WL_CONNECT_FAILED ? "wrong password" :
                s == WL_CONNECTION_LOST? "connection lost" :
                s == WL_DISCONNECTED   ? "disconnected (bad pw or DHCP?)" : "unknown";
            Serial.printf("\nWiFi timeout (status %d: %s)\n", s, reason);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    // Wait for DHCP to assign an address (WL_CONNECTED fires before lease is granted)
    uint32_t dhcp_start = millis();
    while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - dhcp_start < 5000)
        vTaskDelay(pdMS_TO_TICKS(200));
    uint8_t *bssid = WiFi.BSSID();
    Serial.printf("\nConnected to BSSID %02x:%02x:%02x:%02x:%02x:%02x  RSSI %d dBm\n",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], WiFi.RSSI());
    return true;
}

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_MAX_RETRIES 3  // retries before C6 hard reset
#define FETCH_FAIL_RESET_THRESHOLD 10  // consecutive fails before reconnect

static volatile NetType _active_net = NET_NONE;

static AircraftList *_aircraft_list = nullptr;
static uint32_t _last_update = 0;
static TaskHandle_t _fetch_task_handle = nullptr;
static TaskHandle_t _location_poll_task_handle = nullptr;
static FetcherStats _fstats = {};

// Secondary point query for a non-home airport (APRT view). Guarded by its
// own small mutex since it's written from the UI task and read from
// location_fetch_poll (polled from within location_poll_task).
static AircraftList _loc_list;
static SemaphoreHandle_t _loc_target_mutex = nullptr;
static float _loc_target_lat = 0, _loc_target_lon = 0;
static int _loc_target_radius = 0;   // <= 0 means inactive
static uint32_t _loc_target_gen = 0; // bumped whenever the target changes

// Military alert dedup — circular buffer of already-alerted ICAO hexes
#define ALERTED_MAX 64
static char _alerted_hexes[ALERTED_MAX][7];
static int _alerted_count = 0;
static int _alerted_write = 0;

static bool already_alerted(const char *hex) {
    for (int i = 0; i < _alerted_count; i++) {
        if (strcmp(_alerted_hexes[i], hex) == 0) return true;
    }
    return false;
}

static void mark_alerted(const char *hex) {
    strlcpy(_alerted_hexes[_alerted_write], hex, 7);
    _alerted_write = (_alerted_write + 1) % ALERTED_MAX;
    if (_alerted_count < ALERTED_MAX) _alerted_count++;
}

// Check if ICAO hex is in known military ranges
static bool check_military(const char *hex) {
    uint32_t h = strtoul(hex, nullptr, 16);
    // US military (DoD, Army, Navy, USAF)
    if (h >= 0xADF7C8 && h <= 0xAFFFFF) return true;
    // UK military (RAF, Royal Navy, Army Air Corps)
    if (h >= 0x43C000 && h <= 0x43CFFF) return true;
    // France military
    if (h >= 0x3B0000 && h <= 0x3BFFFF) return true;
    // Germany military
    if (h >= 0x3F4000 && h <= 0x3F7FFF) return true;
    // Canada military
    if (h >= 0xC0CDF9 && h <= 0xC0FFFF) return true;
    // Australia military
    if (h >= 0x7C8000 && h <= 0x7CBFFF) return true;
    // NATO/international
    if (h >= 0x0A4000 && h <= 0x0A4FFF) return true;
    return false;
}

static bool check_emergency(uint16_t squawk) {
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

// Returns true for known ADS-B test/synthetic transmitters that should be excluded.
// QPK## = FAA Performance Monitoring Units (Sabre Industries), fixed ground transmitters
//         used to verify ADS-B receiver network coverage — common in western US.
// TEST/TSTR = generic test transponder callsign prefixes.
// 000000 = globally reserved ICAO address, never assigned to a real aircraft.
static bool is_test_signal(const char *hex, const char *callsign) {
    if (strncmp(callsign, "QPK",  3) == 0) return true;
    if (strncmp(callsign, "TEST", 4) == 0) return true;
    if (strncmp(callsign, "TSTR", 4) == 0) return true;
    if (strcmp(hex, "000000")         == 0) return true;
    return false;
}

// Find existing aircraft by ICAO hex, returns index or -1
static int find_aircraft(AircraftList *list, const char *hex) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->aircraft[i].icao_hex, hex) == 0)
            return i;
    }
    return -1;
}

// Pre-parsed aircraft entry — extracted from JSON without holding the lock
struct ParsedEntry {
    char hex[7];
    char callsign[9];
    char registration[9];
    char type_code[5];
    char category[3];
    char desc[40];
    char owner_op[32];
    float lat, lon;
    int32_t altitude;
    int16_t speed, heading, vert_rate;
    uint16_t squawk;
    bool on_ground;
    float mach;
    int16_t ias, tas;
    int32_t nav_altitude;
    float roll;
    float nav_qnh;
};

// Apply a pre-parsed entry to an Aircraft in the main list
static void apply_parsed(Aircraft &a, const ParsedEntry &p, bool is_new) {
    strlcpy(a.icao_hex, p.hex, sizeof(a.icao_hex));
    strlcpy(a.callsign, p.callsign, sizeof(a.callsign));
    strlcpy(a.registration, p.registration, sizeof(a.registration));
    strlcpy(a.type_code, p.type_code, sizeof(a.type_code));
    strlcpy(a.category, p.category, sizeof(a.category));
    strlcpy(a.desc, p.desc, sizeof(a.desc));
    strlcpy(a.owner_op, p.owner_op, sizeof(a.owner_op));
    a.lat = p.lat;
    a.lon = p.lon;
    a.altitude = p.altitude;
    a.speed = p.speed;
    a.heading = p.heading;
    a.vert_rate = p.vert_rate;
    a.squawk = p.squawk;
    a.on_ground = p.on_ground;
    a.mach = p.mach;
    a.ias = p.ias;
    a.tas = p.tas;
    a.nav_altitude = p.nav_altitude;
    a.roll = p.roll;
    a.nav_qnh = p.nav_qnh;
    a.is_military = check_military(a.icao_hex);
    a.is_emergency = check_emergency(a.squawk);
    a.is_watched = false;
    a.last_seen = millis();
    a.stale_since = 0;

    if (is_new) a.trail_count = 0;

    if (a.lat != 0.0f || a.lon != 0.0f) {
        if (a.trail_count < TRAIL_LENGTH) {
            a.trail[a.trail_count] = {a.lat, a.lon, a.altitude, a.last_seen};
            a.trail_count++;
        } else {
            memmove(&a.trail[0], &a.trail[1], (TRAIL_LENGTH - 1) * sizeof(TrailPoint));
            a.trail[TRAIL_LENGTH - 1] = {a.lat, a.lon, a.altitude, a.last_seen};
        }
    }
}

static void parse_aircraft_json(JsonDocument &doc, AircraftList *list, bool do_alerts) {
    JsonArray ac = doc["ac"].as<JsonArray>();

    // Phase 1: Parse JSON into flat array — no lock needed
    static ParsedEntry parsed[MAX_AIRCRAFT];
    int parsed_count = 0;

    for (JsonObject obj : ac) {
        if (parsed_count >= MAX_AIRCRAFT) break;
        float lat = obj["lat"] | 0.0f;
        float lon = obj["lon"] | 0.0f;
        if (lat == 0.0f && lon == 0.0f) continue;

        ParsedEntry &p = parsed[parsed_count];
        strlcpy(p.hex, obj["hex"] | "", sizeof(p.hex));
        strlcpy(p.callsign, obj["flight"] | "", sizeof(p.callsign));
        for (int i = strlen(p.callsign) - 1; i >= 0 && p.callsign[i] == ' '; i--)
            p.callsign[i] = '\0';
        if (is_test_signal(p.hex, p.callsign)) continue;
        strlcpy(p.registration, obj["r"] | "", sizeof(p.registration));
        strlcpy(p.type_code, obj["t"] | "", sizeof(p.type_code));
        strlcpy(p.category, obj["category"] | "", sizeof(p.category));
        strlcpy(p.desc, obj["desc"] | "", sizeof(p.desc));
        strlcpy(p.owner_op, obj["ownOp"] | "", sizeof(p.owner_op));
        p.lat = lat;
        p.lon = lon;
        p.altitude = obj["alt_baro"].is<int>() ? obj["alt_baro"].as<int>() : 0;
        p.speed = (int16_t)(obj["gs"] | 0.0f);
        p.heading = (int16_t)(obj["track"] | 0.0f);
        p.vert_rate = (int16_t)(obj["baro_rate"] | 0.0f);
        p.squawk = strtoul(obj["squawk"] | "0", nullptr, 10);
        p.on_ground = obj["alt_baro"] == "ground";
        p.mach = obj["mach"] | 0.0f;
        p.ias = (int16_t)(obj["ias"] | 0.0f);
        p.tas = (int16_t)(obj["tas"] | 0.0f);
        p.nav_altitude = obj["nav_altitude_mcp"] | 0;
        p.roll = obj["roll"] | 0.0f;
        p.nav_qnh = obj["nav_qnh"] | 0.0f;
        parsed_count++;
    }

    // Phase 2: Brief lock to merge parsed data into aircraft list
    if (!list->lock()) return;

    uint32_t now = millis();
    bool seen[MAX_AIRCRAFT] = {};

    for (int p = 0; p < parsed_count; p++) {
        int idx = find_aircraft(list, parsed[p].hex);
        if (idx >= 0) {
            apply_parsed(list->aircraft[idx], parsed[p], false);
            seen[idx] = true;
        } else if (list->count < MAX_AIRCRAFT) {
            int new_idx = list->count;
            list->aircraft[new_idx].clear();
            apply_parsed(list->aircraft[new_idx], parsed[p], true);
            list->count++;
            seen[new_idx] = true;
        }
    }

    // Mark unseen aircraft as stale, remove expired ghosts
    int write = 0;
    for (int i = 0; i < list->count; i++) {
        Aircraft &a = list->aircraft[i];
        if (!seen[i]) {
            if (a.stale_since == 0) a.stale_since = now;
            if (now - a.stale_since > GHOST_TIMEOUT_MS) continue;
        }
        if (write != i) list->aircraft[write] = list->aircraft[i];
        write++;
    }
    list->count = write;

    // Check for alerts (home list only — a remote APRT-view airport shouldn't page you)
    if (do_alerts) {
        for (int i = 0; i < list->count; i++) {
            Aircraft &a = list->aircraft[i];
            if (a.stale_since != 0) continue;
            if (a.is_emergency && g_config.alert_emergency) {
                char msg[48];
                snprintf(msg, sizeof(msg), "Squawk %04d - %s", a.squawk,
                         a.squawk == 7500 ? "HIJACK" : a.squawk == 7600 ? "COMMS FAIL" : "EMERGENCY");
                alerts_queue(ALERT_EMERGENCY, a.callsign[0] ? a.callsign : a.icao_hex, msg, a.icao_hex);
            } else if (a.is_military && g_config.alert_military && !already_alerted(a.icao_hex)) {
                mark_alerted(a.icao_hex);
                alerts_queue(ALERT_MILITARY, a.callsign[0] ? a.callsign : a.icao_hex, a.type_code, a.icao_hex);
            }
        }
    }

    list->unlock();
    if (list == _aircraft_list) _last_update = millis();
}

static bool network_connected() {
#if defined(USE_ETHERNET)
    if (g_config.use_ethernet) {
        if (ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0)) {
            _active_net = NET_ETHERNET;
            return true;
        }
    } else
#endif
    {
        if (WiFi.status() == WL_CONNECTED) {
            _active_net = NET_WIFI;
            return true;
        }
    }
    _active_net = NET_NONE;
    return false;
}

static void update_ip_addr() {
#if defined(USE_ETHERNET)
    if (_active_net == NET_ETHERNET)
        strlcpy(_fstats.ip_addr, ETH.localIP().toString().c_str(), sizeof(_fstats.ip_addr));
    else
#endif
    if (_active_net == NET_WIFI)
        strlcpy(_fstats.ip_addr, WiFi.localIP().toString().c_str(), sizeof(_fstats.ip_addr));
    else
        strlcpy(_fstats.ip_addr, "N/A", sizeof(_fstats.ip_addr));
}

// On-demand point query for a non-home airport (APRT view). Called from
// location_poll_task's own loop (not a separate task — this project's
// ESP32-P4/C6 combo already runs thin on internal heap, and every extra
// FreeRTOS task stack is internal DRAM taken away from the SDIO/WiFi driver;
// a dedicated task for this crashed the SDIO transport under memory pressure).
// Idles when no target is set; self-throttles to ~15s while a target is
// active, and fetches immediately whenever the target changes.
static uint32_t _loc_last_gen = 0;
static uint32_t _loc_next_fetch = 0;

static void location_fetch_poll() {
    float lat, lon;
    int radius;
    uint32_t gen;
    xSemaphoreTake(_loc_target_mutex, portMAX_DELAY);
    lat = _loc_target_lat;
    lon = _loc_target_lon;
    radius = _loc_target_radius;
    gen = _loc_target_gen;
    xSemaphoreGive(_loc_target_mutex);

    if (radius <= 0) {
        if (_loc_list.count != 0 && _loc_list.lock(pdMS_TO_TICKS(100))) {
            _loc_list.count = 0;
            _loc_list.unlock();
        }
        _loc_last_gen = gen;
        return;
    }

    uint32_t now = millis();
    bool target_changed = (gen != _loc_last_gen);
    if (!target_changed && now < _loc_next_fetch) return;
    _loc_last_gen = gen;
    _loc_next_fetch = now + 15000;

    if (!network_connected()) return;

    if (http_mutex_acquire(pdMS_TO_TICKS(10000))) {
        char url[128];
        snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
                 lat, lon, radius);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(8000);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int content_len = http.getSize();
            size_t buf_size = (content_len > 0) ? (size_t)content_len + 1 : 128 * 1024;
            char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
            size_t total = 0;
            if (buf) {
                size_t target = (content_len > 0) ? (size_t)content_len : buf_size - 1;
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
                        parse_aircraft_json(doc, &_loc_list, false);
                    }
                }
                heap_caps_free(buf);
            }
        }
        http.end();
        http_mutex_release();
    }
}

static void fetch_task(void *param) {
    // Wait for network to come up (with timeout + C6 reset recovery for WiFi)
#if defined(USE_ETHERNET)
    if (g_config.use_ethernet) {
        Serial.print("Fetcher: waiting for Ethernet");
        while (!network_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            Serial.print(".");
        }
    } else
#endif
    {
        int retries = 0;
        while (!network_connected()) {
            if (retries > 0 && retries % WIFI_MAX_RETRIES == 0) {
                Serial.printf("\nWiFi failed %d times, hard-resetting C6\n", retries);
                error_log_add("WiFi stuck, C6 reset #%d", retries / WIFI_MAX_RETRIES);
                reset_wifi_c6();
            }
            Serial.printf("Fetcher: WiFi attempt %d\n", retries + 1);
            if (wifi_connect_with_timeout(WIFI_CONNECT_TIMEOUT_MS)) break;
            retries++;
        }
    }
    update_ip_addr();
    const char *net_name = (_active_net == NET_ETHERNET) ? "Ethernet" : "WiFi";
    Serial.printf("\n%s connected, IP: %s\n", net_name, _fstats.ip_addr);

    // Build API URL
    char url[128];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
             g_config.home_lat, g_config.home_lon, g_config.radius_nm);
    Serial.printf("ADS-B API URL: %s\n", url);

    // Main fetch loop
    int consecutive_fails = 0;
    while (true) {
        if (network_connected()) {
            if (http_mutex_acquire(pdMS_TO_TICKS(15000))) {
                HTTPClient http;
                http.begin(url);
                http.setTimeout(10000);
                uint32_t t0 = millis();
                int httpCode = http.GET();

                if (httpCode == HTTP_CODE_OK) {
                    // Read response into PSRAM buffer (avoids internal heap spike)
                    int content_len = http.getSize();
                    size_t buf_size = (content_len > 0) ? (size_t)content_len + 1 : 256 * 1024;
                    char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
                    size_t total = 0;
                    if (buf) {
                        // Read with deadline loop — single readBytes can return short
                        size_t target = (content_len > 0) ? (size_t)content_len : buf_size - 1;
                        WiFiClient *stream = http.getStreamPtr();
                        uint32_t deadline = millis() + 15000;
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
                    }
                    _fstats.last_fetch_ms = millis() - t0;
                    _fstats.bytes_received += total;

                    if (buf && total > 0) {
                        JsonDocument doc(&_psram_alloc);
                        DeserializationError err = deserializeJson(doc, buf, total);
                        heap_caps_free(buf);
                        if (!err) {
                            parse_aircraft_json(doc, _aircraft_list, true);
                            _fstats.fetch_ok++;
                            consecutive_fails = 0;
                            Serial.printf("Fetched %d ac, heap=%lu\n",
                                _aircraft_list->count,
                                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                        } else {
                            _fstats.fetch_fail++;
                            consecutive_fails++;
                            error_log_add("JSON: %s (%uB)", err.c_str(), total);
                            Serial.printf("JSON error: %s (%u bytes)\n", err.c_str(), total);
                        }
                    } else {
                        if (buf) heap_caps_free(buf);
                        _fstats.fetch_fail++;
                        consecutive_fails++;
                        error_log_add("PSRAM alloc fail / empty resp");
                        Serial.println("PSRAM alloc failed or empty response");
                    }
                } else {
                    _fstats.fetch_fail++;
                    consecutive_fails++;
                    // Don't log routine connection failures (-1) — transient SSL/network
                    if (httpCode != -1) {
                        error_log_add("HTTP %d", httpCode);
                    }
                    Serial.printf("HTTP error: %d\n", httpCode);
                }
                http.end();
                http_mutex_release();
            }
        } else {
            consecutive_fails++;
            error_log_add("Network down");
            Serial.println("Network down, waiting...");
            update_ip_addr();
        }

        // Watchdog: restart only when WiFi itself is down.
        // API outages (WiFi up, server unreachable) just keep retrying — restarting won't help.
        // Resetting only the C6 mid-session corrupts the SDIO bus, so we do a full P4 restart.
        if (!g_config.use_ethernet && consecutive_fails >= FETCH_FAIL_RESET_THRESHOLD) {
            if (!network_connected()) {
                Serial.printf("Watchdog: %d fails, WiFi down — restarting\n", consecutive_fails);
                error_log_add("Watchdog: %d fails, restarting", consecutive_fails);
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP.restart();
            } else {
                Serial.printf("Watchdog: %d API fails, WiFi up — continuing\n", consecutive_fails);
                consecutive_fails = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}

// Background task: fetch route (origin/dest) for aircraft with callsigns
// Was route_enrich_task (adsbdb.com callsign->route lookups) — removed
// entirely, since that data comes from the same VRS Standing Data source
// already documented as unreliable/stale (crowd-sourced, callsign-keyed, no
// versioning — see project_route_data memory). Kept as a lightweight task
// purely to drive location_fetch_poll()/locations_add_poll() on their own
// existing cadence, rather than spawning a new task for them — see
// project_p4_heap_constraints memory for why that matters on this board.
static void location_poll_task(void *param) {
    while (!network_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // let main fetcher populate list first

    while (true) {
        location_fetch_poll();
        locations_add_poll();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void fetcher_set_location_target(float lat, float lon, int radius_nm) {
    xSemaphoreTake(_loc_target_mutex, portMAX_DELAY);
    if (lat != _loc_target_lat || lon != _loc_target_lon || radius_nm != _loc_target_radius) {
        _loc_target_lat = lat;
        _loc_target_lon = lon;
        _loc_target_radius = radius_nm;
        _loc_target_gen++;
    }
    xSemaphoreGive(_loc_target_mutex);
}

AircraftList* fetcher_location_list() {
    return &_loc_list;
}

void fetcher_init(AircraftList *list) {
    _aircraft_list = list;
    _loc_list.init();
    _loc_target_mutex = xSemaphoreCreateMutex();
    http_mutex_init();

    // Only init ONE network stack per boot
#if defined(USE_ETHERNET)
    if (g_config.use_ethernet) {
        ETH.begin();
        Serial.println("Ethernet initialization started");
    } else
#endif
    {
        // Reset C6 module on every boot to ensure clean WiFi state
        // (ESP.restart() only resets the P4, leaving C6 in a potentially bad state)
        reset_wifi_c6();
        Serial.println("WiFi initialization started (C6 reset)");
    }

    xTaskCreatePinnedToCore(fetch_task, "adsb_fetch", 32768, nullptr, 1, &_fetch_task_handle, 1);
    xTaskCreatePinnedToCore(location_poll_task, "loc_poll", 16384, nullptr, 0, &_location_poll_task_handle, 1);
}

bool fetcher_wifi_connected() {
    return network_connected();
}

NetType fetcher_connection_type() {
    return _active_net;
}

uint32_t fetcher_last_update() {
    return _last_update;
}

const FetcherStats* fetcher_get_stats() {
    return &_fstats;
}
