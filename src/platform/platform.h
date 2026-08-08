#pragma once
#include <cstdint>
#include <cstddef>

// Platform-abstraction seam shared between the jc1060 (ESP32-P4, via
// src/platform/esp32/) and Pi (Linux, via pi/platform_linux/) targets.
// data/*.cpp is migrated to call these instead of raw Arduino/FreeRTOS
// APIs so the same source file compiles for both. See
// src/platform/esp32/platform_esp32.cpp for the ESP32 side (thin wrapper
// over what jc1060 already does) and pi/platform_linux/platform_linux.cpp
// for the Linux side.
//
// platform_config_load/save and platform_http_get aren't wired into any
// data/*.cpp file yet (that's task #4/#5 of the Pi port -- see
// project_pi_port memory).

// --- Mutex ---
typedef void *platform_mutex_t;
platform_mutex_t platform_mutex_create();
bool platform_mutex_lock(platform_mutex_t m, uint32_t timeout_ms);
void platform_mutex_unlock(platform_mutex_t m);

// --- Time ---
uint32_t platform_millis();

// --- HTTP ---
// Synchronous GET. Returns true on 2xx with body written into out
// (replacing its contents); false on any failure (caller shouldn't rely on
// out's contents when false is returned).
bool platform_http_get(const char *url, char *out, size_t out_size, size_t *out_len);

// Synchronous GET with optional extra headers and raw HTTP status.
// extra_headers: nullptr, or a nullptr-terminated list of "Name: value"
// strings (libcurl CURLOPT_HTTPHEADER form).
// Returns true when an HTTP response was received (any status); false on
// transport failure. When true, *http_status is set (if non-null) and out
// receives the body (truncated to out_size-1). Callers that only want 2xx
// should keep using platform_http_get().
bool platform_http_get_ex(const char *url, char *out, size_t out_size, size_t *out_len,
                          long *http_status, const char *const *extra_headers);

// --- Config storage ---
// Raw byte blob load/save, keyed by name -- ESP32 side maps this onto an
// NVS blob, Linux side onto a JSON file. UserConfig (data/storage.h) is
// serialized/deserialized by storage.cpp itself either way; this seam only
// deals in bytes.
bool platform_config_load(const char *key, void *buf, size_t buf_size, size_t *out_len);
bool platform_config_save(const char *key, const void *buf, size_t len);

// --- Log ---
void platform_log(const char *fmt, ...);

// --- Compatibility shims ---
// Files that used to `#include <Arduino.h>` directly for just millis()/
// pdMS_TO_TICKS()/strlcpy() now include this header instead. On ESP32 that
// means pulling in the real Arduino.h here so those globals stay available
// exactly as before (zero behavior change); on Linux, the shims below
// stand in for them instead of rewriting every call site during the port.
#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <cstring>

inline uint32_t millis() { return platform_millis(); }

// AircraftList::lock() (data/aircraft.h) takes plain milliseconds on
// Linux, same as the ESP32 side's TickType_t at this project's 1kHz tick
// rate -- this makes existing `list->lock(pdMS_TO_TICKS(N))` call sites
// compile unchanged on both.
#define pdMS_TO_TICKS(ms) (ms)

#if !defined(__APPLE__)
// glibc has no strlcpy -- macOS libc and the ESP32/Arduino toolchain both
// already provide one natively, so this only kicks in for real Linux
// (Pi hardware/VM) builds.
inline size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = (len < size - 1) ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#endif
#endif
