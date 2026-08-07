#include "fetcher.h"
#include "error_log.h"
#include "http_mutex.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include "../data/locations.h"
#include "../data/airlines.h"
#include "../data/enrichment.h"
#include "../data/ota.h"
#include "../data/metar.h"
#include "../data/atis.h"
#include "../ui/alerts.h"
#if defined(USE_ETHERNET)
#include <ETH.h>
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include <esp32-hal-hosted.h> // ESP-Hosted host<->C6 co-processor version check, P4-only (SDIO link) -- see project_p4_heap_constraints memory
#endif

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

    // This used to poll WiFi.status() until it left WL_NO_SHIELD (255), on
    // the theory that's a reliable "SDIO link not up yet" signal. A real
    // boot log proved that wrong: a genuinely fresh, never-touched
    // WiFi.status() on this port reads 254, not 255 -- so the poll's exit
    // condition was never true even on the very first check, and this has
    // silently been a flat ~1.2s delay on every call, never an adaptive
    // wait, since the feature was written. That went unnoticed because the
    // pre-fast-fail retry loop wasted a full 30s per connect attempt
    // regardless, which incidentally gave the hardware plenty of real
    // settle time on top of this. Once retries got fast (see
    // wifi_connect_with_timeout()'s WL_CONNECT_FAILED fast-fail), a real
    // boot hit 3 failed attempts in ~3s, triggered this reset, and the very
    // next WiFi.begin() right after crashed the SDIO transport outright
    // ("Unrecoverable host sdio state" -> hard reboot). There's no
    // verified-reliable readiness signal to poll instead (hostedIsInitialized()
    // exists but its behavior at this exact call site -- before WiFi.mode()
    // has ever run -- is unverified, and guessing wrong here risks the same
    // class of bug again), so erring generous on a flat delay is the safer
    // call. status is still logged for whatever diagnostic value it has.
    uint32_t t0 = millis();
    vTaskDelay(pdMS_TO_TICKS(3000));
    Serial.printf("C6 reset complete (%lums, status=%d)\n", millis() - t0, (int)WiFi.status());
}

// Attempt WiFi connection with timeout. Returns true if connected.
static bool wifi_connect_with_timeout(uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.setScanMethod(WIFI_FAST_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    Serial.printf("WiFi connecting to '%s'...\n", g_config.wifi_ssid);
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_pass);

    auto reason_for = [](wl_status_t s) -> const char * {
        // WL_CONNECT_FAILED does NOT mean "wrong password" specifically --
        // it's the generic auth/handshake-failed status, and empirically
        // fires reliably on the very first connect attempt right after a
        // fresh C6 reset, then succeeds on the next attempt seconds later
        // with the same unchanged credentials. Mislabeling it "wrong
        // password" sent a wrong signal here once already -- don't repeat
        // that. A *persistent* WL_CONNECT_FAILED across retries is still
        // worth checking the password for; a single one right after reset
        // isn't evidence of that on its own.
        return s == WL_NO_SSID_AVAIL  ? "SSID not found" :
               s == WL_CONNECT_FAILED ? "auth/handshake failed (often transient right after a C6 reset)" :
               s == WL_CONNECTION_LOST? "connection lost" :
               s == WL_DISCONNECTED   ? "disconnected (bad pw, DHCP, or AP out of range?)" : "unknown";
    };

    uint32_t start = millis();
    int last_logged = -1; // sentinel -- wl_status_t is always a non-negative byte value
    while (WiFi.status() != WL_CONNECTED) {
        wl_status_t s = WiFi.status();
        // Log every status *change* (not every 500ms tick) -- previously
        // this loop never looked at status at all until the outer timeout
        // fired, so there was no way to tell whether a terminal failure
        // showed up seconds in (and we then sat out the rest of the timeout
        // pointlessly) or only right at the deadline (meaning the C6/IDF
        // driver itself is what's actually slow here). This settles that
        // question either way, without changing behavior yet.
        if ((int)s != last_logged) {
            Serial.printf("\n  [WiFi] status -> %d at %lums\n", (int)s, (unsigned long)(millis() - start));
            last_logged = (int)s;
        }
        // Bail out the moment the driver reports a *terminal* failure rather
        // than sitting out the rest of timeout_ms regardless -- WL_CONNECT_FAILED
        // and WL_NO_SSID_AVAIL only ever get set once the connect attempt is
        // actually done and failed (not a transient in-progress state), so
        // there's nothing to wait for. The caller's retry loop can start a
        // fresh WiFi.begin() immediately instead.
        if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) {
            Serial.printf("WiFi failed fast after %lums (status %d: %s)\n",
                (unsigned long)(millis() - start), s, reason_for(s));
            return false;
        }
        if (millis() - start > timeout_ms) {
            Serial.printf("\nWiFi timeout after %lums (status %d: %s)\n",
                (unsigned long)(millis() - start), s, reason_for(s));
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

#ifdef CONFIG_IDF_TARGET_ESP32P4
    // Read-only version check -- does NOT trigger hostedUpdate()/reflash, just
    // logs whether the C6 co-processor firmware still matches what this
    // Arduino-ESP32 core expects. A drift here (e.g. from a future platform
    // bump on the P4 side without re-running the C6 updater) is a known
    // contributor to SDIO instability -- see project_p4_heap_constraints memory.
    uint32_t hmaj, hmin, hpat, smaj, smin, spat;
    hostedGetHostVersion(&hmaj, &hmin, &hpat);
    hostedGetSlaveVersion(&smaj, &smin, &spat);
    Serial.printf("ESP-Hosted versions: host v%lu.%lu.%lu, C6 co-processor v%lu.%lu.%lu%s\n",
        hmaj, hmin, hpat, smaj, smin, spat,
        hostedHasUpdate() ? "  <-- MISMATCH, co-processor firmware is out of date" : "");
#endif

    return true;
}

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_MAX_RETRIES 3  // retries before C6 hard reset
#define FETCH_FAIL_RESET_THRESHOLD 10  // consecutive fails before reconnect

// NetworkClientSecure's default TLS handshake timeout is 120s and is NOT
// bounded by HTTPClient::setTimeout() (that only covers the read phase after
// a connection succeeds) -- a slow/hung handshake here would otherwise hold
// http_mutex for up to two full minutes, starving every other network
// consumer in the app (enrichment, the saved-location poll, add-airport).
// Must construct the WiFiClientSecure ourselves and call
// setHandshakeTimeout() on it before HTTPClient::begin(), since the
// single-string begin(url) overload creates its own client with the 120s
// default baked in.
#define TLS_HANDSHAKE_TIMEOUT_S 8

// HTTPClient only exposes headers explicitly registered via collectHeaders()
// before GET() -- without this, http.header("Retry-After") always returns
// empty even if adsb.lol sends one, silently skipping straight to the
// exponential-backoff fallback on a 429. Shared by both adsb.lol pollers.
static const char *kAdsbLolHeaders[] = {"Retry-After"};

static volatile NetType _active_net = NET_NONE;

static AircraftList *_aircraft_list = nullptr;
static uint32_t _last_update = 0;
static TaskHandle_t _fetch_task_handle = nullptr;
static TaskHandle_t _location_poll_task_handle = nullptr;
static FetcherStats _fstats = {};

// Given by locations_set_active() (via fetcher_request_immediate_fetch())
// whenever the active location changes; fetch_task's loop takes-with-timeout
// on this instead of a bare vTaskDelay, so switching locations wakes it
// immediately rather than leaving the view showing stale/no data until the
// next scheduled ~20s tick. Binary, not counting -- multiple switches before
// the loop gets back around to waiting just leave it signaled once, which
// just means one (harmless, already-fresh) extra fetch, not a bug.
static SemaphoreHandle_t _fetch_now_sem = nullptr;

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
    bool vert_rate_valid; // false when the source JSON simply omitted baro_rate this cycle -- not the same as a confirmed 0fpm/level reading
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
    a.vert_rate_valid = p.vert_rate_valid;
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
        p.vert_rate_valid = !obj["baro_rate"].isNull();
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

    // Check for alerts. There's only one active feed now (see the fetch-loop
    // consolidation this replaced -- no more separate Home-only vs. saved-
    // location paths), so alerts fire for whichever location is currently
    // active, not just a privileged "Home". If you want some locations to
    // never page you, that's a new, separate feature to design -- not
    // something this collapsed automatically.
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
    _last_update = millis();
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
    if (!g_config.wifi_ssid[0]) {
        // Nothing to connect to -- a factory-reset or never-configured
        // device has an empty SSID here, and no amount of retrying (or
        // resetting the C6) will ever change that. Falling into the retry
        // loop below unconditionally used to fast-fail almost instantly
        // against a blank SSID and hit its "reset the C6 every
        // WIFI_MAX_RETRIES failures" branch within seconds, over and over
        // (reported: a factory-reset device visibly spinning through
        // repeated connect-fail-reset cycles with no way to stop it) --
        // exactly the tight WiFi.begin()-retry-with-no-gap pattern that's
        // already crashed the SDIO transport once before (see
        // reset_wifi_c6()'s comment for that history). WiFi credentials are
        // only ever read at boot (settings.cpp/serial_config.cpp), so the
        // only way out of this state is setting them and rebooting -- there
        // is nothing productive to retry in the meantime.
        //
        // Still calling WiFi.mode(WIFI_STA) here, even though nothing will
        // actually connect -- skipping it entirely crashed on real hardware
        // ("assert failed: tcpip_send_msg_wait_sem ... Invalid mbox"):
        // network_connected()'s WiFi.status() and update_ip_addr()'s
        // WiFi.localIP() just below both go through esp-idf's lwIP/netif
        // layer, which doesn't exist until WiFi.mode() brings it up --
        // calling into it beforehand hits an uninitialized task mailbox and
        // panics. WiFi.mode(WIFI_STA) alone doesn't attempt a connection or
        // touch the C6 beyond the already-completed reset in fetcher_init(),
        // so it doesn't reopen the retry/reset risk this branch exists to
        // avoid -- it's the same first line wifi_connect_with_timeout()
        // always calls anyway, just without the WiFi.begin() after it.
        WiFi.mode(WIFI_STA);
        Serial.println("Fetcher: no WiFi SSID configured -- skipping connection attempts. Set one in Settings (gear icon) or via tools/configure_device.sh, then reboot.");
        error_log_add("No WiFi configured -- set it in Settings");
    } else
    {
        int retries = 0;
        while (!network_connected()) {
            if (retries > 0 && retries % WIFI_MAX_RETRIES == 0) {
                Serial.printf("\nWiFi failed %d times, hard-resetting C6\n", retries);
                error_log_add("WiFi stuck, C6 reset #%d", retries / WIFI_MAX_RETRIES);
                reset_wifi_c6();
            } else if (retries > 0) {
                // wifi_connect_with_timeout()'s fast-fail on WL_CONNECT_FAILED
                // means a failed attempt can now return in ~0-3s instead of
                // the old flat 30s -- good for boot time, but a real boot
                // showed back-to-back attempts firing with zero gap between
                // them (two consecutive 1ms failures, each just re-reading
                // the same still-latched status rather than actually
                // retrying), which turned out to be part of what led to an
                // SDIO transport crash shortly after (see reset_wifi_c6()'s
                // comment for the full chain). A short pause costs almost
                // nothing against the old 30s/attempt baseline but gives the
                // link a moment to actually settle between attempts.
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            Serial.printf("Fetcher: WiFi attempt %d\n", retries + 1);
            if (wifi_connect_with_timeout(WIFI_CONNECT_TIMEOUT_MS)) break;
            retries++;
        }
    }
    // Refresh _active_net before reading it -- the retry loop above exits
    // via wifi_connect_with_timeout()'s own success return (break), without
    // re-calling network_connected() (the only thing that actually updates
    // _active_net). Without this, _active_net is still stuck at NET_NONE
    // from the loop's last failing iteration, so update_ip_addr() below
    // would print "N/A" here even though the connection just succeeded --
    // this was the 100%-reproducible-at-boot version of the N/A bug (as
    // opposed to the reconnect-staleness one fixed by the per-tick call in
    // the main loop below).
    network_connected();
    update_ip_addr();
    // Previously unconditional -- safe when every path above this point
    // only ever fell through after a real connection succeeded (Ethernet's
    // loop has no other exit; the old WiFi loop only broke out on success).
    // The empty-SSID branch above changed that: it deliberately falls
    // through without connecting, and this line printing "WiFi connected,
    // IP: N/A" right after "no WiFi SSID configured" on a real boot is what
    // first made the crash below it obvious in the log -- fixing the
    // message alone doesn't fix the crash (see the empty-SSID branch's own
    // comment for that), but it should still say the true thing.
    if (_active_net != NET_NONE) {
        const char *net_name = (_active_net == NET_ETHERNET) ? "Ethernet" : "WiFi";
        Serial.printf("\n%s connected, IP: %s\n", net_name, _fstats.ip_addr);
    } else {
        Serial.println("\nNo network connection");
    }

    // One-time load of the airline code->name lookup table (see airlines.h) —
    // done here, once, on this task's existing stack rather than a new task.
    airlines_load();

    // Main fetch loop
    int consecutive_fails = 0;
    int consecutive_429s = 0; // adsb.lol rate-limiting us -- see backoff below
    while (true) {
        uint32_t extra_delay_ms = 0;
        if (network_connected()) {
            // Refresh every tick, not just once at boot -- otherwise a
            // transient disconnect (which sets ip_addr to "N/A" below) never
            // gets corrected once the connection recovers, since nothing
            // else re-populates the real address after a reconnect.
            update_ip_addr();
            float lat, lon;
            if (locations_get_active_coords(&lat, &lon, nullptr) &&
                http_mutex_acquire(pdMS_TO_TICKS(15000))) {
                // Built fresh every poll rather than once before this loop
                // started -- otherwise switching the active location (or
                // changing its coordinates) in the picker/settings would
                // re-center the map/radar views instantly (each view's own
                // per-tick active-location-change detection does that) while
                // the actual ADS-B query kept silently hitting the old
                // location/radius until reboot.
                char url[128];
                snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
                         lat, lon, g_config.radius_nm);
                WiFiClientSecure client;
                client.setInsecure(); // matches http.begin(url)'s own no-CA-cert behavior
                client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
                HTTPClient http;
                http.begin(client, url);
                http.collectHeaders(kAdsbLolHeaders, 1);
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
                            consecutive_429s = 0;
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
                    if (httpCode == 429) {
                        // adsb.lol rate-limits dynamically based on their own
                        // server load (no fixed published quota) -- back off
                        // instead of hammering it again in 20s regardless.
                        // Respect Retry-After if they send one; otherwise
                        // double the normal cadence per consecutive 429,
                        // capped at 5 minutes.
                        consecutive_429s++;
                        String retry_after = http.header("Retry-After");
                        int retry_secs = retry_after.length() ? retry_after.toInt() : 0;
                        if (retry_secs > 0) {
                            extra_delay_ms = (uint32_t)retry_secs * 1000;
                        } else {
                            uint32_t mult = 1UL << (consecutive_429s > 4 ? 4 : consecutive_429s);
                            extra_delay_ms = 20000UL * mult;
                            if (extra_delay_ms > 300000UL) extra_delay_ms = 300000UL;
                        }
                        error_log_add("HTTP 429, backing off %lus", (unsigned long)(extra_delay_ms / 1000));
                        Serial.printf("HTTP 429 (rate limited) -- backing off %lus\n",
                            (unsigned long)(extra_delay_ms / 1000));
                    } else {
                        consecutive_429s = 0;
                        // Don't log routine connection failures (-1) — transient SSL/network
                        if (httpCode != -1) {
                            error_log_add("HTTP %d", httpCode);
                        }
                        Serial.printf("HTTP error: %d\n", httpCode);
                    }
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
        //
        // wifi_ssid[0] guard added alongside the empty-SSID skip above --
        // without it, a factory-reset/never-configured device (no WiFi
        // attempted at all, network_connected() permanently false) would
        // hit this threshold every ~200s and restart itself in an endless
        // loop. Same reasoning as the API-outage case just above: restarting
        // doesn't fix "nothing to connect to" any more than it fixes a
        // server-side outage, so there's nothing to gain by trying.
        if (!g_config.use_ethernet && g_config.wifi_ssid[0] && consecutive_fails >= FETCH_FAIL_RESET_THRESHOLD) {
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

        // Wait out the normal cadence, but wake early if
        // fetcher_request_immediate_fetch() signals a location switch --
        // see _fetch_now_sem's declaration for why this is a semaphore
        // wait rather than a bare vTaskDelay.
        uint32_t cadence_ms = extra_delay_ms > 20000 ? extra_delay_ms : 20000;
        xSemaphoreTake(_fetch_now_sem, pdMS_TO_TICKS(cadence_ms));
    }
}

// Background task: originally route_enrich_task (adsbdb.com callsign->route
// lookups) — removed entirely, since that data comes from the same VRS
// Standing Data source already documented as unreliable/stale (crowd-
// sourced, callsign-keyed, no versioning — see project_route_data memory).
// Kept as a lightweight task purely to drive locations_add_poll()/
// locations_nearby_poll()/enrichment_poll()/ota_poll()/metar_poll()/
// locations_verify_token_poll() on their own existing cadence, rather than
// spawning a new task for them — see project_p4_heap_constraints memory for
// why that matters on this board.
static void location_poll_task(void *param) {
    while (!network_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // let main fetcher populate list first

    while (true) {
        locations_add_poll();
        locations_nearby_poll(); // nearby-large-airport runway cache -- one queued fetch per tick, same cadence as locations_add_poll()
        locations_verify_token_poll(); // TOKEN_VERIFY (serial_config.cpp) -- idle unless a check was actually requested
        enrichment_poll(); // detail-card aircraft/photo lookups -- see enrichment.cpp
        ota_poll(); // application-firmware update check/download -- see ota.cpp. Near-instant unless a check/update was actually requested (rare, user-triggered), in which case this tick runs long -- acceptable, see ota.h's comment.
        metar_poll(); // nearest-station weather readout -- see metar.h. Internally rate-limited (active-location change or every 10min), near-instant otherwise.
        atis_poll();  // D-ATIS for active airport ICAO -- see atis.h. Stub on ESP32 until WiFi port.
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void fetcher_request_immediate_fetch() {
    // A location switch means every aircraft currently in the list is for
    // an entirely different place -- without this, they stuck around for
    // the usual GHOST_TIMEOUT_MS (30s) fade instead of disappearing
    // immediately, which visibly ghosted the old location's traffic across
    // the new location's Map/Radar for a while, and (the concrete report)
    // kept getting counted into the new location's UNIQUE/PEAK/CLOSEST
    // stats until they finally aged out -- reading as "stats don't reset
    // on switch" even though stats.cpp's own counters were reset correctly.
    // A location switch is a hard cut, not a fade, so the list is cleared
    // outright rather than left to expire on its own schedule.
    if (_aircraft_list && _aircraft_list->lock(pdMS_TO_TICKS(50))) {
        _aircraft_list->count = 0;
        _aircraft_list->unlock();
    }
    if (_fetch_now_sem) xSemaphoreGive(_fetch_now_sem);
}

void fetcher_init(AircraftList *list) {
    _aircraft_list = list;
    _fetch_now_sem = xSemaphoreCreateBinary();
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
