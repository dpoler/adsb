#include "airlines.h"
#include "http_mutex.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <cstring>

#define AIRLINES_URL "https://raw.githubusercontent.com/dpoler/AirlinesCSV/main/airlines.csv"

// NetworkClientSecure's default TLS handshake timeout is 120s and is NOT
// bounded by HTTPClient::setTimeout() -- see fetcher.cpp for the full
// explanation. This runs once at boot, but a hung handshake here would
// still hold http_mutex (and delay the very first ADS-B fetch) for up to
// two minutes instead of failing promptly.
#define TLS_HANDSHAKE_TIMEOUT_S 8

static AirlineEntry _airlines[AIRLINES_MAX];
static int _airline_count = 0;

// Parses one CSV line: ICAO,Name[,Callsign]  (# comments and blank lines skipped)
static void parse_line(const char *line, size_t len) {
    if (len == 0 || line[0] == '#' || _airline_count >= AIRLINES_MAX) return;

    const char *c1 = (const char *)memchr(line, ',', len);
    if (!c1) return;
    size_t icao_len = c1 - line;
    if (icao_len < 2 || icao_len > 3) return;

    const char *rest = c1 + 1;
    size_t rest_len = len - icao_len - 1;
    const char *c2 = (const char *)memchr(rest, ',', rest_len);
    size_t name_len = c2 ? (size_t)(c2 - rest) : rest_len;
    if (name_len == 0) return;

    AirlineEntry &e = _airlines[_airline_count];
    memset(&e, 0, sizeof(e));
    memcpy(e.code, line, icao_len < 3 ? icao_len : 3);
    memcpy(e.name, rest, name_len < 25 ? name_len : 25);
    if (c2) {
        const char *cs = c2 + 1;
        size_t cs_len = rest_len - name_len - 1;
        memcpy(e.callsign, cs, cs_len < 15 ? cs_len : 15);
    }
    _airline_count++;
}

bool airlines_load() {
    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) return false;

    WiFiClientSecure client;
    client.setInsecure(); // matches http.begin(url)'s own no-CA-cert behavior
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
    HTTPClient http;
    http.begin(client, AIRLINES_URL);
    http.setTimeout(15000);
    uint32_t t0 = millis();
    int code = http.GET();
    Serial.printf("[Airlines] HTTP %d in %lums\n", code, (unsigned long)(millis() - t0));

    bool ok = false;
    if (code == HTTP_CODE_OK) {
        int len = http.getSize();
        size_t buf_size = (len > 0) ? (size_t)len + 1 : 32 * 1024;
        char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t total = 0;
            size_t target = (len > 0) ? (size_t)len : buf_size - 1;
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

            _airline_count = 0;
            size_t line_start = 0;
            for (size_t i = 0; i <= total; i++) {
                if (i == total || buf[i] == '\n') {
                    size_t line_len = i - line_start;
                    if (line_len > 0 && buf[line_start + line_len - 1] == '\r') line_len--;
                    parse_line(buf + line_start, line_len);
                    line_start = i + 1;
                }
            }
            heap_caps_free(buf);
            ok = _airline_count > 0;
            Serial.printf("[Airlines] Loaded %d entries\n", _airline_count);
        }
    }

    http.end();
    http_mutex_release();
    return ok;
}

const AirlineEntry *airline_lookup(const char *callsign) {
    if (!callsign) return nullptr;
    char prefix[4] = {0, 0, 0, 0};
    int plen = 0;
    while (plen < 3 && callsign[plen] >= 'A' && callsign[plen] <= 'Z') {
        prefix[plen] = callsign[plen];
        plen++;
    }
    if (plen < 2) return nullptr;

    for (int i = 0; i < _airline_count; i++) {
        if (strcmp(prefix, _airlines[i].code) == 0) return &_airlines[i];
    }
    return nullptr;
}
