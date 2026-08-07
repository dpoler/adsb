// Pi D-ATIS fetch via https://datis.clowd.io/api/{ICAO} (no key).
// ESP32 stub lives in pi/app_stubs.cpp until a WiFiClientSecure port exists.

#include "../../src/data/atis.h"
#include "../../src/data/locations.h"
#include "../../src/platform/platform.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>

#define ATIS_REFRESH_MS (5UL * 60UL * 1000UL)

volatile AtisStatus atis_status = ATIS_IDLE;
AtisReport atis_reports[ATIS_MAX_REPORTS] = {};
int atis_report_count = 0;

static void clear_reports() {
    atis_report_count = 0;
    for (int i = 0; i < ATIS_MAX_REPORTS; i++) {
        atis_reports[i].type[0] = '\0';
        atis_reports[i].code[0] = '\0';
        atis_reports[i].text[0] = '\0';
    }
}

static void uppercase_icao(char *dst, size_t dstlen, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < dstlen; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static const char *type_label(const char *type) {
    if (!type) return "ATIS";
    if (strcmp(type, "arr") == 0) return "ARR";
    if (strcmp(type, "dep") == 0) return "DEP";
    return "ATIS";
}

static void do_fetch(const char *icao) {
    atis_status = ATIS_FETCHING;

    char icao_u[LOC_ICAO_LEN];
    uppercase_icao(icao_u, sizeof(icao_u), icao);

    char url[96];
    snprintf(url, sizeof(url), "https://datis.clowd.io/api/%s", icao_u);

    constexpr size_t BUF = 16 * 1024;
    char *body = new (std::nothrow) char[BUF];
    if (!body) {
        atis_status = ATIS_ERROR;
        platform_log("[ATIS] OOM\n");
        return;
    }
    size_t len = 0;
    bool ok = platform_http_get(url, body, BUF, &len);
    if (!ok) {
        delete[] body;
        // 404 / empty often means no D-ATIS for this field.
        clear_reports();
        atis_status = ATIS_NONE;
        platform_log("[ATIS] %s: no D-ATIS (or fetch failed)\n", icao_u);
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, len);
    delete[] body;
    if (err) {
        atis_status = ATIS_ERROR;
        platform_log("[ATIS] JSON parse error: %s\n", err.c_str());
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) {
        clear_reports();
        atis_status = ATIS_NONE;
        platform_log("[ATIS] %s: empty\n", icao_u);
        return;
    }

    clear_reports();
    int n = 0;
    for (JsonObject obj : arr) {
        if (n >= ATIS_MAX_REPORTS) break;
        const char *type = obj["type"] | "combined";
        const char *code = obj["code"] | "";
        const char *text = obj["datis"] | "";
        if (!text[0]) continue;
        strlcpy(atis_reports[n].type, type, sizeof(atis_reports[n].type));
        strlcpy(atis_reports[n].code, code, sizeof(atis_reports[n].code));
        strlcpy(atis_reports[n].text, text, sizeof(atis_reports[n].text));
        n++;
    }

    if (n == 0) {
        atis_status = ATIS_NONE;
        platform_log("[ATIS] %s: no text\n", icao_u);
        return;
    }

    atis_report_count = n;
    atis_status = ATIS_OK;
    platform_log("[ATIS] %s: %d report(s) (%s%s)\n", icao_u, n,
                 type_label(atis_reports[0].type),
                 n > 1 ? "+…" : "");
}

void atis_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2;
    static char last_icao[LOC_ICAO_LEN] = "";

    int idx = locations_active_index();
    if (idx < 0) {
        if (last_loc_idx != -1) {
            atis_status = ATIS_IDLE;
            clear_reports();
            last_loc_idx = -1;
            last_icao[0] = '\0';
        }
        return;
    }

    const Location *loc = locations_get(idx);
    if (!loc || !loc->icao[0]) {
        // Waypoint — no ATIS.
        if (last_loc_idx != idx || last_icao[0]) {
            atis_status = ATIS_IDLE;
            clear_reports();
            last_loc_idx = idx;
            last_icao[0] = '\0';
        }
        return;
    }

    uint32_t now = platform_millis();
    bool loc_changed = (idx != last_loc_idx) || (strcmp(last_icao, loc->icao) != 0);
    bool due = (now - last_fetch_ms >= ATIS_REFRESH_MS);
    if (!loc_changed && !due) return;

    if (loc_changed) {
        clear_reports();
    }

    last_loc_idx = idx;
    strlcpy(last_icao, loc->icao, sizeof(last_icao));
    last_fetch_ms = now;
    do_fetch(loc->icao);
}
