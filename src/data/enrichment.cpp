#include "enrichment.h"
#include "http_mutex.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <cstring>
#include "lvgl.h"

// PSRAM allocator for ArduinoJson — keeps internal RAM free
struct EnrichPsramAlloc : ArduinoJson::Allocator {
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
static EnrichPsramAlloc _enrich_alloc;

#define MAX_CACHE 20

static AircraftEnrichment _cache[MAX_CACHE];
static char _cache_keys[MAX_CACHE][7];
static int _cache_count = 0;

// Two-stage fetch (adsbdb details, then planespotters photo), driven by
// enrichment_poll() from location_poll_task's existing loop (fetcher.cpp)
// instead of a dedicated xTaskCreatePinnedToCore() per tap -- spawning a new
// task for every detail-card tap was the same anti-pattern that caused the
// SDIO crashes fixed in the location-picker "add by ICAO" flow (see
// project_p4_heap_constraints memory). The poll loop's own ~1.5s cadence
// already gives stage 2 a breather after stage 1 without needing an
// explicit delay -- each stage runs on its own poll tick.
enum EnrichStage { STAGE_IDLE, STAGE_1_PENDING, STAGE_2_PENDING };
static volatile EnrichStage _stage = STAGE_IDLE;
static char _pending_icao[7];
static void (*_pending_callback)(AircraftEnrichment *) = nullptr;
static AircraftEnrichment *_active_entry = nullptr;

// Deferred callback — set by enrichment_poll(), delivered by LVGL timer
static volatile AircraftEnrichment *_deferred_entry = nullptr;
static volatile bool _deferred_ready = false;

AircraftEnrichment *enrichment_get_cached(const char *icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0 && _cache[i].loaded) {
            return &_cache[i];
        }
    }
    return nullptr;
}

static AircraftEnrichment *get_or_create_cache_entry(const char *icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0) return &_cache[i];
    }
    int idx = _cache_count < MAX_CACHE ? _cache_count++ : 0;
    memset(&_cache[idx], 0, sizeof(AircraftEnrichment));
    strlcpy(_cache_keys[idx], icao_hex, 7);
    return &_cache[idx];
}

// Signal LVGL context to call the callback (thread-safe)
static void notify_callback(AircraftEnrichment *entry) {
    _deferred_entry = entry;
    _deferred_ready = true;
}

// Read HTTP response into PSRAM buffer, parse with PSRAM allocator
static bool fetch_and_parse(HTTPClient &http, JsonDocument &doc) {
    int len = http.getSize();
    size_t buf_size = (len > 0 && len < 16384) ? (size_t)len + 1 : 8192;
    char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) { http.end(); return false; }

    size_t total = 0;
    WiFiClient *stream = http.getStreamPtr();
    uint32_t deadline = millis() + 8000;
    size_t target = (len > 0) ? (size_t)len : buf_size - 1;
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
    http.end();

    bool ok = !deserializeJson(doc, buf, total);
    heap_caps_free(buf);
    return ok;
}

static void run_stage1(AircraftEnrichment *entry) {
    if (http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        char url[128];
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", _pending_icao);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(5000);
        if (http.GET() == HTTP_CODE_OK) {
            JsonDocument doc(&_enrich_alloc);
            if (fetch_and_parse(http, doc)) {
                JsonObject ac = doc["response"]["aircraft"];
                strlcpy(entry->manufacturer, ac["manufacturer"] | "", sizeof(entry->manufacturer));
                strlcpy(entry->model, ac["type"] | "", sizeof(entry->model));
                strlcpy(entry->registered_country, ac["registered_owner_country_name"] | "",
                        sizeof(entry->registered_country));
                entry->engine_count = ac["engine_count"] | 0;
                strlcpy(entry->engine_type, ac["engine_type"] | "", sizeof(entry->engine_type));
                entry->year_built = ac["year_built"] | 0;
            }
        } else {
            http.end();
        }
        http_mutex_release();
        notify_callback(entry);
    }
}

static void run_stage2(AircraftEnrichment *entry) {
    if (http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        char url[128];
        snprintf(url, sizeof(url),
                 "https://api.planespotters.net/pub/photos/hex/%s", _pending_icao);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(5000);
        if (http.GET() == HTTP_CODE_OK) {
            JsonDocument doc(&_enrich_alloc);
            if (fetch_and_parse(http, doc)) {
                JsonArray photos = doc["photos"].as<JsonArray>();
                if (photos.size() > 0) {
                    strlcpy(entry->photo_url, photos[0]["thumbnail_large"]["src"] | "", sizeof(entry->photo_url));
                    strlcpy(entry->photo_photographer, photos[0]["photographer"] | "", sizeof(entry->photo_photographer));
                }
            }
        } else {
            http.end();
        }
        http_mutex_release();
    }

    entry->loaded = true;
    entry->loading = false;
    notify_callback(entry);
}

// Called from location_poll_task's existing loop (fetcher.cpp), ~every 1.5s.
void enrichment_poll() {
    if (_stage == STAGE_1_PENDING) {
        run_stage1(_active_entry);
        _stage = STAGE_2_PENDING; // runs on the next tick -- no explicit delay needed
    } else if (_stage == STAGE_2_PENDING) {
        run_stage2(_active_entry);
        _stage = STAGE_IDLE;
        _active_entry = nullptr;
    }
}

void enrichment_fetch(const char *icao_hex, const char *registration,
                      void (*callback)(AircraftEnrichment *data)) {
    // Check cache first
    AircraftEnrichment *cached = enrichment_get_cached(icao_hex);
    if (cached) {
        callback(cached);
        return;
    }

    // Only one enrichment fetch at a time
    if (_stage != STAGE_IDLE) {
        Serial.println("enrich: skipped (fetch already in progress)");
        return;
    }

    AircraftEnrichment *entry = get_or_create_cache_entry(icao_hex);
    entry->loading = true;

    _pending_callback = callback;
    _active_entry = entry;
    strlcpy(_pending_icao, icao_hex, sizeof(_pending_icao));
    _deferred_ready = false;
    _stage = STAGE_1_PENDING;
}

// Call from LVGL context (main.cpp setup) to install the deferred callback timer
void enrichment_init() {
    lv_timer_create([](lv_timer_t *t) {
        if (_deferred_ready && _pending_callback && _deferred_entry) {
            _deferred_ready = false;
            _pending_callback((AircraftEnrichment *)_deferred_entry);
        }
    }, 200, nullptr);
}
