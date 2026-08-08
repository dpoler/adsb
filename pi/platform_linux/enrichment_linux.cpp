// Linux implementation of src/data/enrichment.h -- same two API sources as
// src/data/enrichment.cpp (adsbdb.com aircraft details, then
// planespotters.net photo metadata), plus Pi-only steps: download + decode
// the JPEG thumbnail into RGB565 for the detail card, and (optional)
// AeroDataBox flight status for live origin/destination. Pi has no PSRAM
// cache-coherency erratum, so real photos work.
//
// Threading: one detached std::thread per detail-card tap (Pi threads are
// cheap; ESP32's poll-from-existing-task constraint does not apply). UI
// callbacks are deferred through an LVGL timer, same as the ESP32 side.

#include "../../src/data/enrichment.h"
#include "../../src/data/storage.h"
#include "../../src/platform/platform.h"
#include "lvgl.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ctime>

// stb_image implementation lives in basemap.cpp -- only the declarations here.
#include "../third_party/stb_image.h"

#define MAX_CACHE 20
// Sized to detail_card.cpp's PHOTO_SLOT on 1280-wide layouts (400x220).
// Mild shrink from thumbnail_large (~500x280) with bilinear.
#define PHOTO_MAX_W 400
#define PHOTO_MAX_H 220

namespace {

std::mutex _mutex;
AircraftEnrichment _cache[MAX_CACHE];
char _cache_keys[MAX_CACHE][7];
int _cache_count = 0;
bool _busy = false;

void (*_pending_callback)(AircraftEnrichment *) = nullptr;
volatile AircraftEnrichment *_deferred_entry = nullptr;
volatile bool _deferred_ready = false;

void free_photo(AircraftEnrichment *e) {
    free(e->photo_rgb565);
    e->photo_rgb565 = nullptr;
    e->photo_w = 0;
    e->photo_h = 0;
}

AircraftEnrichment *get_or_create_cache_entry(const char *icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0) return &_cache[i];
    }
    int idx = _cache_count < MAX_CACHE ? _cache_count++ : 0;
    free_photo(&_cache[idx]);
    memset(&_cache[idx], 0, sizeof(AircraftEnrichment));
    strlcpy(_cache_keys[idx], icao_hex, 7);
    return &_cache[idx];
}

void notify_callback(AircraftEnrichment *entry) {
    _deferred_entry = entry;
    _deferred_ready = true;
}

bool http_get_json(const char *url, JsonDocument &doc) {
    std::vector<char> buf(48 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) return false;
    return deserializeJson(doc, buf.data(), len) == DeserializationError::Ok;
}

// RapidAPI / API.Market / Direct gateway helpers for AeroDataBox.
// OpenAPI FlightSearchByEnum uses PascalCase (Icao24/CallSign/Reg/Number).
enum AdboxProvider { ADBOX_RAPIDAPI = 0, ADBOX_APIMARKET = 1, ADBOX_DIRECT = 2 };

const char *adbox_base_url(int provider) {
    switch (provider) {
    case ADBOX_APIMARKET: return "https://prod.api.market/api/v1/aedbx/aerodatabox";
    case ADBOX_DIRECT:    return "https://api.aerodatabox.com";
    default:              return "https://aerodatabox.p.rapidapi.com";
    }
}

// Fills hdr_bufs[0]/ and out[] (nullptr-terminated). key/provider from config.
void adbox_headers(int provider, const char *key,
                   char hdr_bufs[2][160], const char *out[4]) {
    out[0] = out[1] = out[2] = out[3] = nullptr;
    int n = 0;
    switch (provider) {
    case ADBOX_APIMARKET:
        snprintf(hdr_bufs[0], 160, "x-api-market-key: %s", key);
        out[n++] = hdr_bufs[0];
        break;
    case ADBOX_DIRECT:
        snprintf(hdr_bufs[0], 160, "X-Api-Key: %s", key);
        out[n++] = hdr_bufs[0];
        break;
    default: // RapidAPI
        snprintf(hdr_bufs[0], 160, "x-rapidapi-key: %s", key);
        snprintf(hdr_bufs[1], 160, "x-rapidapi-host: aerodatabox.p.rapidapi.com");
        out[n++] = hdr_bufs[0];
        out[n++] = hdr_bufs[1];
        break;
    }
    out[n++] = "Accept: application/json";
    out[n] = nullptr;
}

std::string trim_ws(const char *s) {
    if (!s) return {};
    while (*s && isspace((unsigned char)*s)) ++s;
    std::string out(s);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    return out;
}

std::string alnum_upper(const char *s) {
    std::string clean;
    for (const char *p = s; p && *p; ++p) {
        if (isalnum((unsigned char)*p)) clean.push_back((char)toupper((unsigned char)*p));
    }
    return clean;
}

void today_utc_ymd(char *out, size_t out_sz) {
    time_t now = time(nullptr);
    struct tm tm_utc {};
    gmtime_r(&now, &tm_utc);
    snprintf(out, out_sz, "%04d-%02d-%02d",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
}

int current_yyyymm() {
    time_t now = time(nullptr);
    struct tm tm_utc {};
    gmtime_r(&now, &tm_utc);
    return (tm_utc.tm_year + 1900) * 100 + (tm_utc.tm_mon + 1);
}

// Roll the monthly counter if needed, then record one AeroDataBox HTTP call.
// On 429 (or soft-limit breach) auto-disables the service and persists.
void adbox_note_call(long http_status) {
    int ym = current_yyyymm();
    if (g_config.adbox_usage_yyyymm != ym) {
        g_config.adbox_usage_yyyymm = ym;
        g_config.adbox_usage_count = 0;
        // New UTC calendar month — clear sticky rate-limit so the user can try again.
        if (g_config.adbox_rate_limited) {
            g_config.adbox_rate_limited = false;
        }
    }
    g_config.adbox_usage_count++;

    bool hit_soft = (g_config.adbox_soft_limit > 0
                     && g_config.adbox_usage_count >= g_config.adbox_soft_limit);
    bool hit_429 = (http_status == 429);
    if ((hit_soft || hit_429) && g_config.aerodatabox_enabled) {
        g_config.aerodatabox_enabled = false;
        g_config.adbox_rate_limited = true;
        platform_log("[Enrich] AeroDataBox auto-disabled (%s, count=%d limit=%d)\n",
                     hit_429 ? "HTTP 429" : "soft limit",
                     g_config.adbox_usage_count, g_config.adbox_soft_limit);
        storage_save_config(g_config);
    } else if ((g_config.adbox_usage_count % 5) == 0) {
        // Persist periodically so a crash doesn't lose the whole month's count.
        storage_save_config(g_config);
    }
}

bool adbox_allowed() {
    if (!g_config.aerodatabox_enabled || !g_config.aerodatabox_key[0]) return false;
    if (g_config.adbox_rate_limited) return false;
    int ym = current_yyyymm();
    if (g_config.adbox_soft_limit > 0
        && g_config.adbox_usage_yyyymm == ym
        && g_config.adbox_usage_count >= g_config.adbox_soft_limit) {
        return false;
    }
    return true;
}

bool parse_adbox_route(JsonDocument &doc, char *origin, size_t origin_sz,
                       char *dest, size_t dest_sz) {
    // Response is a JSON array of FlightContract, or occasionally a single object.
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    JsonObjectConst flight;
    if (!arr.isNull()) {
        if (arr.size() == 0) return false;
        // Prefer a flight that has both airports when several are returned.
        for (JsonObjectConst f : arr) {
            const char *o = f["departure"]["airport"]["icao"] | "";
            const char *d = f["arrival"]["airport"]["icao"] | "";
            if (o[0] && d[0]) { flight = f; break; }
            if (flight.isNull() && (o[0] || d[0])) flight = f;
        }
        if (flight.isNull()) flight = arr[0].as<JsonObjectConst>();
    } else {
        flight = doc.as<JsonObjectConst>();
    }
    if (flight.isNull()) return false;

    const char *o = flight["departure"]["airport"]["icao"] | "";
    const char *d = flight["arrival"]["airport"]["icao"] | "";
    if (!o[0] && !d[0]) {
        o = flight["departure"]["airport"]["iata"] | "";
        d = flight["arrival"]["airport"]["iata"] | "";
    }
    if (!o[0] && !d[0]) return false;
    strlcpy(origin, o, origin_sz);
    strlcpy(dest, d, dest_sz);
    return true;
}

// Live nearest flight, then same search for today's local date. searchBy uses
// OpenAPI PascalCase (Icao24 / CallSign / Reg).
bool fetch_adbox_route(int provider, const char *key,
                       const char *icao_hex, const char *callsign, const char *registration,
                       char *origin, size_t origin_sz, char *dest, size_t dest_sz) {
    origin[0] = '\0';
    dest[0] = '\0';
    if (!key || !key[0]) return false;

    char hdr_bufs[2][160];
    const char *hdrs[4];
    adbox_headers(provider, key, hdr_bufs, hdrs);
    const char *base = adbox_base_url(provider);

    char today[16];
    today_utc_ymd(today, sizeof(today));

    auto try_url = [&](const char *url) -> bool {
        std::vector<char> buf(96 * 1024);
        size_t len = 0;
        long status = 0;
        if (!platform_http_get_ex(url, buf.data(), buf.size(), &len, &status, hdrs)) {
            platform_log("[Enrich] AeroDataBox transport fail: %s\n", url);
            adbox_note_call(0);
            return false;
        }
        adbox_note_call(status);
        if (status == 401 || status == 403) {
            platform_log("[Enrich] AeroDataBox auth failed (http=%ld)\n", status);
            return false;
        }
        if (status == 429) {
            platform_log("[Enrich] AeroDataBox rate limited (http=429)\n");
            return false;
        }
        if (status == 204 || status == 404) {
            platform_log("[Enrich] AeroDataBox no flight (http=%ld) %s\n", status, url);
            return false;
        }
        if (status < 200 || status >= 300) {
            platform_log("[Enrich] AeroDataBox http=%ld len=%zu %s\n", status, len, url);
            return false;
        }
        if (len == 0) return false;
        JsonDocument doc;
        if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
            platform_log("[Enrich] AeroDataBox JSON parse fail (%zu bytes)\n", len);
            return false;
        }
        return parse_adbox_route(doc, origin, origin_sz, dest, dest_sz);
    };

    auto try_search = [&](const char *search_by, const char *param) -> bool {
        if (!param || !param[0]) return false;
        char url[320];
        // Nearest (no date) -- preferred for live traffic.
        snprintf(url, sizeof(url),
                 "%s/flights/%s/%s?withAircraftImage=false&withLocation=false",
                 base, search_by, param);
        if (try_url(url)) return true;
        // Same-day dated search (UTC date; dateLocalRole=Both).
        snprintf(url, sizeof(url),
                 "%s/flights/%s/%s/%s?withAircraftImage=false&withLocation=false&dateLocalRole=Both",
                 base, search_by, param, today);
        return try_url(url);
    };

    std::string hex = alnum_upper(icao_hex);
    if (!hex.empty() && try_search("Icao24", hex.c_str())) return true;

    std::string cs = alnum_upper(trim_ws(callsign).c_str());
    if (!cs.empty() && try_search("CallSign", cs.c_str())) return true;

    // Registration: keep letters/digits only (drop dashes/spaces).
    std::string reg = alnum_upper(registration);
    if (!reg.empty() && try_search("Reg", reg.c_str())) return true;

    return false;
}

// Validate key with a cheap airport lookup (not a flight search).
bool validate_adbox_key(int provider, const char *key, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };
    if (!key || !key[0]) return fail("no key set");

    char hdr_bufs[2][160];
    const char *hdrs[4];
    adbox_headers(provider, key, hdr_bufs, hdrs);

    char buf[4096];
    size_t len = 0;
    long status = 0;
    char url[256];
    snprintf(url, sizeof(url), "%s/airports/icao/KJFK", adbox_base_url(provider));
    if (!platform_http_get_ex(url, buf, sizeof(buf), &len, &status, hdrs)) {
        adbox_note_call(0);
        return fail("network error");
    }
    adbox_note_call(status);
    if (status == 401 || status == 403) return fail("invalid key");
    if (status == 429) return fail("rate limited");
    if (status < 200 || status >= 300) {
        char msg[48];
        snprintf(msg, sizeof(msg), "http %ld", status);
        return fail(msg);
    }
    return true;
}

std::mutex _verify_mutex;
bool _verify_result_ready = false;
bool _verify_result_ok = false;
char _verify_result_err[48] = {};

bool http_get_bytes(const char *url, std::vector<uint8_t> &out) {
    // thumbnail_large is typically ~30-80KB.
    std::vector<char> buf(512 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len) || len == 0) return false;
    out.assign((uint8_t *)buf.data(), (uint8_t *)buf.data() + len);
    return true;
}

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint8_t sample_channel(const unsigned char *rgb, int w, int h,
                                     float x, float y, int c) {
    // Bilinear sample of channel c in an RGB888 buffer.
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > (float)(w - 1)) x = (float)(w - 1);
    if (y > (float)(h - 1)) y = (float)(h - 1);
    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    const unsigned char *p00 = rgb + ((size_t)y0 * (size_t)w + (size_t)x0) * 3 + c;
    const unsigned char *p10 = rgb + ((size_t)y0 * (size_t)w + (size_t)x1) * 3 + c;
    const unsigned char *p01 = rgb + ((size_t)y1 * (size_t)w + (size_t)x0) * 3 + c;
    const unsigned char *p11 = rgb + ((size_t)y1 * (size_t)w + (size_t)x1) * 3 + c;
    float v = (1 - fx) * (1 - fy) * (float)(*p00)
            + fx * (1 - fy) * (float)(*p10)
            + (1 - fx) * fy * (float)(*p01)
            + fx * fy * (float)(*p11);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v + 0.5f);
}

// Decode JPEG/PNG to RGB565. Keeps near-native size; bilinear when scaling.
bool decode_photo_rgb565(const uint8_t *bytes, size_t len,
                         uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h) {
    int w = 0, h = 0, n = 0;
    unsigned char *rgb = stbi_load_from_memory(bytes, (int)len, &w, &h, &n, 3);
    if (!rgb || w <= 0 || h <= 0) {
        if (rgb) stbi_image_free(rgb);
        return false;
    }

    int dw = w, dh = h;
    if (dw > PHOTO_MAX_W || dh > PHOTO_MAX_H) {
        float sx = (float)PHOTO_MAX_W / (float)dw;
        float sy = (float)PHOTO_MAX_H / (float)dh;
        float s = sx < sy ? sx : sy;
        dw = (int)(dw * s + 0.5f);
        dh = (int)(dh * s + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    size_t bytes_out = (size_t)dw * (size_t)dh * 2;
    uint8_t *rgb565 = (uint8_t *)malloc(bytes_out);
    if (!rgb565) {
        stbi_image_free(rgb);
        return false;
    }

    // Ordered 2x2 dither softens RGB565 banding (reads as grain on sky/fuselage).
    static const float dither[2][2] = {
        { 0.0f / 4.0f, 2.0f / 4.0f },
        { 3.0f / 4.0f, 1.0f / 4.0f },
    };

    const bool scale = (dw != w) || (dh != h);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint8_t r, g, b;
            if (!scale) {
                const unsigned char *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
                r = p[0]; g = p[1]; b = p[2];
            } else {
                float sx = ((float)x + 0.5f) * (float)w / (float)dw - 0.5f;
                float sy = ((float)y + 0.5f) * (float)h / (float)dh - 0.5f;
                r = sample_channel(rgb, w, h, sx, sy, 0);
                g = sample_channel(rgb, w, h, sx, sy, 1);
                b = sample_channel(rgb, w, h, sx, sy, 2);
            }
            float d = dither[y & 1][x & 1];
            int ri = (int)r + (int)(d * 7.0f);
            int gi = (int)g + (int)(d * 3.0f);
            int bi = (int)b + (int)(d * 7.0f);
            if (ri > 255) ri = 255;
            if (gi > 255) gi = 255;
            if (bi > 255) bi = 255;
            uint16_t pix = rgb888_to_rgb565((uint8_t)ri, (uint8_t)gi, (uint8_t)bi);
            size_t off = ((size_t)y * (size_t)dw + (size_t)x) * 2;
            rgb565[off] = (uint8_t)(pix & 0xFF);
            rgb565[off + 1] = (uint8_t)(pix >> 8);
        }
    }
    stbi_image_free(rgb);
    *out_rgb = rgb565;
    *out_w = (uint16_t)dw;
    *out_h = (uint16_t)dh;
    return true;
}

void run_enrichment(std::string icao, std::string registration, std::string callsign) {
    AircraftEnrichment *entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        entry = get_or_create_cache_entry(icao.c_str());
        entry->loading = true;
    }

    // --- Stage 1: adsbdb aircraft details ---
    {
        char url[128];
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", icao.c_str());
        JsonDocument doc;
        if (http_get_json(url, doc)) {
            JsonObjectConst ac = doc["response"]["aircraft"];
            std::lock_guard<std::mutex> lock(_mutex);
            strlcpy(entry->manufacturer, ac["manufacturer"] | "", sizeof(entry->manufacturer));
            strlcpy(entry->model, ac["type"] | "", sizeof(entry->model));
            strlcpy(entry->registered_country, ac["registered_owner_country_name"] | "",
                    sizeof(entry->registered_country));
            entry->engine_count = ac["engine_count"] | 0;
            strlcpy(entry->engine_type, ac["engine_type"] | "", sizeof(entry->engine_type));
            entry->year_built = ac["year_built"] | 0;
        } else {
            platform_log("[Enrich] stage1 (adsbdb) failed for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    // --- Stage 2: planespotters photo metadata ---
    {
        char url[160];
        snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", icao.c_str());
        JsonDocument doc;
        bool got = http_get_json(url, doc);
        JsonArrayConst photos = got ? doc["photos"].as<JsonArrayConst>() : JsonArrayConst{};
        if ((!got || photos.size() == 0) && !registration.empty()) {
            // Fallback: some airframes are keyed by registration only.
            snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/reg/%s",
                     registration.c_str());
            got = http_get_json(url, doc);
            photos = got ? doc["photos"].as<JsonArrayConst>() : JsonArrayConst{};
        }
        if (got && photos.size() > 0) {
            std::lock_guard<std::mutex> lock(_mutex);
            strlcpy(entry->photo_url, photos[0]["thumbnail_large"]["src"] | "",
                    sizeof(entry->photo_url));
            if (!entry->photo_url[0]) {
                strlcpy(entry->photo_url, photos[0]["thumbnail"]["src"] | "",
                        sizeof(entry->photo_url));
            }
            strlcpy(entry->photo_photographer, photos[0]["photographer"] | "",
                    sizeof(entry->photo_photographer));
        } else {
            platform_log("[Enrich] stage2 (planespotters) no photos for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    // --- Stage 3: download + decode thumbnail (Pi-only opportunity) ---
    char photo_url[256] = {};
    {
        std::lock_guard<std::mutex> lock(_mutex);
        strlcpy(photo_url, entry->photo_url, sizeof(photo_url));
    }
    if (photo_url[0]) {
        std::vector<uint8_t> jpeg;
        if (http_get_bytes(photo_url, jpeg)) {
            uint8_t *rgb = nullptr;
            uint16_t pw = 0, ph = 0;
            if (decode_photo_rgb565(jpeg.data(), jpeg.size(), &rgb, &pw, &ph)) {
                std::lock_guard<std::mutex> lock(_mutex);
                free_photo(entry);
                entry->photo_rgb565 = rgb;
                entry->photo_w = pw;
                entry->photo_h = ph;
                platform_log("[Enrich] photo %ux%u for %s\n", pw, ph, icao.c_str());
            } else {
                platform_log("[Enrich] photo decode failed for %s\n", icao.c_str());
            }
        } else {
            platform_log("[Enrich] photo download failed for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    // --- Stage 4: AeroDataBox live origin/destination (optional) ---
    char adbox_key[sizeof(g_config.aerodatabox_key)] = {};
    int adbox_prov = 0;
    bool adbox_on = false;
    {
        adbox_on = adbox_allowed();
        adbox_prov = g_config.aerodatabox_provider;
        if (adbox_on) strlcpy(adbox_key, g_config.aerodatabox_key, sizeof(adbox_key));
    }
    if (adbox_on) {
        char origin[8] = {}, dest[8] = {};
        if (fetch_adbox_route(adbox_prov, adbox_key, icao.c_str(), callsign.c_str(),
                              registration.c_str(),
                              origin, sizeof(origin), dest, sizeof(dest))) {
            std::lock_guard<std::mutex> lock(_mutex);
            strlcpy(entry->origin_icao, origin, sizeof(entry->origin_icao));
            strlcpy(entry->dest_icao, dest, sizeof(entry->dest_icao));
            entry->route_checked = true;
            platform_log("[Enrich] route %s -> %s for %s\n", origin, dest, icao.c_str());
        } else {
            std::lock_guard<std::mutex> lock(_mutex);
            entry->route_checked = true;
            platform_log("[Enrich] AeroDataBox no route for %s\n", icao.c_str());
        }
        notify_callback(entry);
    } else {
        std::lock_guard<std::mutex> lock(_mutex);
        entry->route_checked = true; // skipped — don't re-hit until cache cleared
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        entry->loaded = true;
        entry->loading = false;
        _busy = false;
    }
    notify_callback(entry);
}

} // namespace

AircraftEnrichment *enrichment_get_cached(const char *icao_hex) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0 && _cache[i].loaded) {
            return &_cache[i];
        }
    }
    return nullptr;
}

void enrichment_poll() {
    // No-op -- Pi drives stages on a dedicated thread from enrichment_fetch().
}

void enrichment_fetch(const char *icao_hex, const char *registration,
                      const char *callsign,
                      void (*callback)(AircraftEnrichment *data)) {
    if (!icao_hex || !icao_hex[0]) return;

    const bool adbox_on = adbox_allowed();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (int i = 0; i < _cache_count; i++) {
            if (strcmp(_cache_keys[i], icao_hex) != 0 || !_cache[i].loaded) continue;
            // Cache hit — but if AeroDataBox is on and we never checked route
            // (e.g. service was enabled after a prior enrich), refresh.
            if (adbox_on && !_cache[i].route_checked) {
                break; // fall through to full re-fetch
            }
            _pending_callback = callback;
            notify_callback(&_cache[i]);
            return;
        }
        if (_busy) {
            platform_log("enrich: skipped (fetch already in progress)\n");
            return;
        }
        _busy = true;
        _pending_callback = callback;
        _deferred_ready = false;
    }

    std::string icao(icao_hex);
    std::string reg(registration ? registration : "");
    std::string cs(callsign ? callsign : "");
    std::thread([icao, reg, cs]() { run_enrichment(icao, reg, cs); }).detach();
}

void enrichment_clear_cache() {
    std::lock_guard<std::mutex> lock(_mutex);
    for (int i = 0; i < _cache_count; i++) {
        free_photo(&_cache[i]);
        memset(&_cache[i], 0, sizeof(AircraftEnrichment));
        _cache_keys[i][0] = '\0';
    }
    _cache_count = 0;
}

void aerodatabox_request_verify() {
    {
        std::lock_guard<std::mutex> lock(_verify_mutex);
        _verify_result_ready = false;
    }
    std::thread([]() {
        char err[48] = {};
        bool ok = validate_adbox_key(g_config.aerodatabox_provider, g_config.aerodatabox_key,
                                     err, sizeof(err));
        std::lock_guard<std::mutex> lock(_verify_mutex);
        _verify_result_ok = ok;
        strlcpy(_verify_result_err, err, sizeof(_verify_result_err));
        _verify_result_ready = true;
    }).detach();
}

bool aerodatabox_verify_result(bool *ok, char *err, size_t err_size) {
    std::lock_guard<std::mutex> lock(_verify_mutex);
    if (!_verify_result_ready) return false;
    if (ok) *ok = _verify_result_ok;
    if (err && err_size) strlcpy(err, _verify_result_err, err_size);
    _verify_result_ready = false;
    return true;
}

void aerodatabox_usage_snapshot(int *yyyymm, int *count, int *soft_limit, bool *rate_limited) {
    int ym = current_yyyymm();
    int n = (g_config.adbox_usage_yyyymm == ym) ? g_config.adbox_usage_count : 0;
    if (yyyymm) *yyyymm = ym;
    if (count) *count = n;
    if (soft_limit) *soft_limit = g_config.adbox_soft_limit;
    if (rate_limited) *rate_limited = g_config.adbox_rate_limited;
}

void aerodatabox_clear_rate_limit() {
    g_config.adbox_rate_limited = false;
    storage_save_config(g_config);
}

void enrichment_init() {
    lv_timer_create([](lv_timer_t *t) {
        if (_deferred_ready && _pending_callback && _deferred_entry) {
            _deferred_ready = false;
            _pending_callback((AircraftEnrichment *)_deferred_entry);
        }
    }, 100, nullptr);
}
