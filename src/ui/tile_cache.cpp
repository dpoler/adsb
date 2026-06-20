#include "tile_cache.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <cmath>
#include <cstring>
#include "esp_cache.h"

// lodepng is compiled by LVGL when LV_USE_LODEPNG is enabled
extern "C" {
    unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                              const unsigned char *in, size_t insize);
}

// --- OSM tile math ---

int osm_lon_to_x(float lon, int z) {
    return (int)floorf((lon + 180.0f) / 360.0f * (float)(1 << z));
}

int osm_lat_to_y(float lat, int z) {
    float lat_rad = lat * M_PI / 180.0f;
    return (int)floorf((1.0f - logf(tanf(lat_rad) + 1.0f / cosf(lat_rad)) / M_PI) / 2.0f * (float)(1 << z));
}

float osm_x_to_lon(int x, int z) {
    return (float)x / (float)(1 << z) * 360.0f - 180.0f;
}

float osm_y_to_lat(int y, int z) {
    float n = M_PI - 2.0f * M_PI * (float)y / (float)(1 << z);
    return 180.0f / M_PI * atanf(0.5f * (expf(n) - expf(-n)));
}

int osm_zoom_for_radius(float radius_nm, int screen_h, float center_lat) {
    float our_ppd = (float)screen_h / (radius_nm * 2.0f) * 60.0f;
    float cos_lat = cosf(center_lat * M_PI / 180.0f);

    int best_z = 4;
    float best_diff = 1e9f;
    for (int z = 4; z < 16; z++) {
        float osm_ppd = 256.0f * (float)(1 << z) / 360.0f * cos_lat;
        float diff = fabsf(logf(osm_ppd / our_ppd));
        if (diff < best_diff) {
            best_diff = diff;
            best_z = z;
        }
    }
    return best_z;
}

// Forward declaration
static void destroy_draw_buf(lv_draw_buf_t *dbuf);

// --- PSRAM tile cache ---

#define MAX_CACHED_TILES 48

struct CachedTile {
    int z, x, y;
    lv_draw_buf_t *draw_buf;
    uint32_t last_used;
    bool valid;
    bool empty;          // fetched but no content (404, ocean, etc.)
};

static CachedTile _tiles[MAX_CACHED_TILES];
static SemaphoreHandle_t _cache_mutex;
static QueueHandle_t _fetch_queue;
static bool _sd_available = false;

struct FetchRequest {
    int z, x, y;
};

static CachedTile *find_tile(int z, int x, int y) {
    for (int i = 0; i < MAX_CACHED_TILES; i++) {
        if (_tiles[i].valid && _tiles[i].z == z && _tiles[i].x == x && _tiles[i].y == y) {
            _tiles[i].last_used = millis();
            return &_tiles[i];
        }
    }
    return nullptr;
}

static CachedTile *evict_lru() {
    int oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;
    for (int i = 0; i < MAX_CACHED_TILES; i++) {
        if (!_tiles[i].valid) return &_tiles[i];
        if (_tiles[i].last_used < oldest_time) {
            oldest_time = _tiles[i].last_used;
            oldest_idx = i;
        }
    }
    return &_tiles[oldest_idx];
}

static void cache_insert(int z, int x, int y, lv_draw_buf_t *draw_buf, bool empty) {
    if (xSemaphoreTake(_cache_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (draw_buf) destroy_draw_buf(draw_buf);
        return;
    }

    CachedTile *slot = find_tile(z, x, y);
    if (slot) {
        xSemaphoreGive(_cache_mutex);
        if (draw_buf) destroy_draw_buf(draw_buf);
        return;
    }

    slot = evict_lru();
    if (slot->draw_buf) {
        destroy_draw_buf(slot->draw_buf);
        slot->draw_buf = nullptr;
    }
    slot->z = z;
    slot->x = x;
    slot->y = y;
    slot->draw_buf = draw_buf;
    slot->last_used = millis();
    slot->valid = true;
    slot->empty = empty;
    xSemaphoreGive(_cache_mutex);
}

// --- PNG decode into lv_draw_buf ---

// Tile stride: TILE_PX * 2 bytes per row (RGB565, no padding)
#define TILE_STRIDE (TILE_PX * 2)
#define TILE_BUF_SIZE (TILE_STRIDE * TILE_PX)

static lv_draw_buf_t *decode_png_to_draw_buf(const uint8_t *png, size_t len) {
    unsigned char *rgba = nullptr;
    unsigned w, h;
    unsigned err = lodepng_decode32(&rgba, &w, &h, png, len);
    if (err || w != TILE_PX || h != TILE_PX) {
        if (rgba) free(rgba);
        return nullptr;
    }

    // Allocate draw buf struct + pixel buffer separately for cache control
    lv_draw_buf_t *dbuf = (lv_draw_buf_t *)lv_malloc_zeroed(sizeof(lv_draw_buf_t));
    if (!dbuf) { free(rgba); return nullptr; }

    // Allocate pixel data in PSRAM
    uint8_t *pixels = (uint8_t *)heap_caps_malloc(
        TILE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) { lv_free(dbuf); free(rgba); return nullptr; }

    free(rgba);

    // TEST: solid blue - if tiles render as blue, pipeline works
    memset(pixels, 0x1F, TILE_BUF_SIZE);

    // Flush CPU data cache to PSRAM so any core/DMA can read
    esp_cache_msync(pixels, TILE_BUF_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    // Initialize the draw buf header
    dbuf->header.magic = LV_IMAGE_HEADER_MAGIC;
    dbuf->header.cf = LV_COLOR_FORMAT_RGB565;
    dbuf->header.w = TILE_PX;
    dbuf->header.h = TILE_PX;
    dbuf->header.stride = TILE_STRIDE;
    dbuf->header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    dbuf->data = pixels;
    dbuf->unaligned_data = pixels;
    dbuf->data_size = TILE_BUF_SIZE;
    return dbuf;
}

static void destroy_draw_buf(lv_draw_buf_t *dbuf) {
    if (!dbuf) return;
    if (dbuf->data) heap_caps_free(dbuf->data);
    lv_free(dbuf);
}

// --- SD card cache ---

static void sd_cache_path(int z, int x, int y, char *buf, size_t len) {
    snprintf(buf, len, "/tiles/%d/%d/%d.png", z, x, y);
}

static lv_draw_buf_t *sd_load_tile(int z, int x, int y) {
    if (!_sd_available) return nullptr;

    char path[64];
    sd_cache_path(z, x, y, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return nullptr;

    size_t size = f.size();
    if (size == 0 || size > 200000) { f.close(); return nullptr; }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return nullptr; }

    f.read(buf, size);
    f.close();

    lv_draw_buf_t *dbuf = decode_png_to_draw_buf(buf, size);
    free(buf);
    return dbuf;
}

static void sd_save_tile(int z, int x, int y, const uint8_t *png, size_t len) {
    if (!_sd_available) return;

    char dir[48];
    snprintf(dir, sizeof(dir), "/tiles/%d", z);
    SD_MMC.mkdir(dir);
    snprintf(dir, sizeof(dir), "/tiles/%d/%d", z, x);
    SD_MMC.mkdir(dir);

    char path[64];
    sd_cache_path(z, x, y, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_WRITE);
    if (f) {
        f.write(png, len);
        f.close();
    }
}

// --- Background fetch task ---

static void fetch_task(void *param) {
    FetchRequest req;
    char url[128];

    while (true) {
        if (xQueueReceive(_fetch_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        // Skip if already cached
        if (xSemaphoreTake(_cache_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool cached = find_tile(req.z, req.x, req.y) != nullptr;
            xSemaphoreGive(_cache_mutex);
            if (cached) continue;
        }

        // Try SD card first
        lv_draw_buf_t *dbuf = sd_load_tile(req.z, req.x, req.y);
        if (dbuf) {
            cache_insert(req.z, req.x, req.y, dbuf, false);
            continue;
        }

        // Fetch from network
        if (WiFi.status() != WL_CONNECTED) continue;

        snprintf(url, sizeof(url),
            "https://basemaps.cartocdn.com/dark_all/%d/%d/%d.png",
            req.z, req.x, req.y);

        HTTPClient http;
        http.begin(url);
        http.addHeader("User-Agent", "ADS-B-Display/1.0");
        http.setTimeout(10000);
        int code = http.GET();

        if (code == HTTP_CODE_OK) {
            int content_len = http.getSize();
            if (content_len > 0 && content_len < 200000) {
                uint8_t *png = (uint8_t *)heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
                if (png) {
                    WiFiClient *stream = http.getStreamPtr();
                    int total = 0;
                    uint32_t timeout = millis() + 10000;
                    while (total < content_len && millis() < timeout) {
                        size_t avail = stream->available();
                        if (avail > 0) {
                            int n = stream->readBytes(png + total,
                                min((int)avail, content_len - total));
                            total += n;
                        } else {
                            vTaskDelay(pdMS_TO_TICKS(5));
                        }
                    }

                    if (total == content_len) {
                        sd_save_tile(req.z, req.x, req.y, png, content_len);
                        dbuf = decode_png_to_draw_buf(png, content_len);
                        if (dbuf) {
                            cache_insert(req.z, req.x, req.y, dbuf, false);
                        }
                    }
                    free(png);
                }
            }
        } else if (code == 404) {
            cache_insert(req.z, req.x, req.y, nullptr, true);
        }

        http.end();

        // Rate limit: 200ms between fetches
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --- Public API ---

void tile_cache_init() {
    _cache_mutex = xSemaphoreCreateMutex();
    _fetch_queue = xQueueCreate(32, sizeof(FetchRequest));
    memset(_tiles, 0, sizeof(_tiles));

#if 0
    _sd_available = SD_MMC.begin("/sdcard", true);
    if (_sd_available) {
        SD_MMC.mkdir("/tiles");
        Serial.println("Tile cache: SD card ready");
    } else {
        Serial.println("Tile cache: no SD card, network-only caching");
    }
#else
    _sd_available = false;
    Serial.println("Tile cache: SD disabled (ESP32-P4), network-only caching");
#endif

    // Pin to core 0 (same as LVGL) to avoid cross-core cache coherency issues
    // with PSRAM pixel data on ESP32-P4 RISC-V
    xTaskCreatePinnedToCore(fetch_task, "tile_fetch", 32768, nullptr, 1, nullptr, 0);
}

lv_draw_buf_t *tile_cache_get(int z, int x, int y) {
    if (!_cache_mutex) return nullptr;
    if (xSemaphoreTake(_cache_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return nullptr;

    CachedTile *t = find_tile(z, x, y);
    lv_draw_buf_t *result = nullptr;
    bool found = false;
    if (t) {
        found = true;
        if (!t->empty) result = t->draw_buf;
    }
    xSemaphoreGive(_cache_mutex);

    if (!found) {
        FetchRequest req = {z, x, y};
        xQueueSend(_fetch_queue, &req, 0);
    }

    return result;
}

void tile_cache_flush_queue() {
    if (_fetch_queue) xQueueReset(_fetch_queue);
}
