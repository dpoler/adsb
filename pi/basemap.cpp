// Pi Map basemap: multi-style tile mosaic (Carto dark / FAA VFR sectional),
// disk-cached RGB565, async fetch. Lives under pi/ so PlatformIO never
// compiles it into the jc1060 firmware.
//
// Threading model (important — two past crash sources):
// 1) Never call LVGL alloc/decode from the worker. LVGL's heap is not safe
//    off the LVGL thread; the bundled lodepng is patched to use it. Decode
//    with stb_image (plain malloc) on the worker instead (PNG + JPEG).
// 2) Never mutate the drawn buffer while LVGL SW draw units may still be
//    reading it (LV_DRAW_SW_DRAW_UNIT_CNT>1 queues work). Worker publishes
//    into an inbox; the LVGL thread installs it via basemap_poll_swap()
//    between frames.
//
// Cache: one RGB565 file per (style, lat, lon, range, canvas geometry).
// Freshness is mtime vs a per-style TTL (OSM/Carto ~30d; sectionals ~40d
// so we refetch ahead of the ~56-day chart cycle). Filename includes an
// `eq5` tag: HTTP/1.1 tile fetch (fixes OpenTopo missing squares under HTTP/2).

#include "basemap.h"

#include "../src/platform/platform.h"
#include "../src/ui/display_prefs.h"

#include <curl/curl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

namespace {

constexpr int TILE_PX = 256;

// FAA ArcGIS VFR_Sectional MapServer only publishes LOD 8..12.
constexpr int SECTIONAL_Z_MIN = 8;
constexpr int SECTIONAL_Z_MAX = 12;

struct BasemapSlot {
    std::vector<uint8_t> rgb565;
    lv_image_dsc_t dsc{};
    float lat = 0, lon = 0, radius_nm = 0;
    int w = 0, h = 0;
    int geo_cy = 0;
    int bullseye_r = 0;
    int style = MAP_BASEMAP_STYLE_DARK;
    bool valid = false;

    void bind_dsc() {
        memset(&dsc, 0, sizeof(dsc));
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = w;
        dsc.header.h = h;
        dsc.header.stride = w * 2;
        dsc.data_size = (uint32_t)rgb565.size();
        dsc.data = rgb565.data();
    }
};

std::mutex g_mu;
BasemapSlot g_front;          // LVGL thread only (after install)
BasemapSlot g_inbox;          // worker → LVGL publish slot
bool g_inbox_ready = false;
bool g_worker_busy = false;
float g_req_lat = 0, g_req_lon = 0, g_req_radius = 0;
int g_req_w = 0, g_req_h = 0, g_req_cy = 0, g_req_br = 0;
int g_req_style = MAP_BASEMAP_STYLE_DARK;
uint32_t g_req_gen = 0;

// UI progress (network/build only — cache hits stay silent).
bool g_prog_visible = false;
int g_prog_pct = 0; // 0..100

void progress_set(uint32_t gen, bool visible, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    std::lock_guard<std::mutex> lock(g_mu);
    if (gen != g_req_gen) return;
    g_prog_visible = visible;
    g_prog_pct = pct;
}

void progress_clear_if_gen(uint32_t gen) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (gen == g_req_gen) {
        g_prog_visible = false;
        g_prog_pct = 0;
    }
}

bool slot_matches(const BasemapSlot &s, float lat, float lon, float radius_nm,
                  int w, int h, int cy, int br, int style) {
    return s.valid && s.style == style &&
           fabsf(s.lat - lat) < 1e-4f && fabsf(s.lon - lon) < 1e-4f &&
           fabsf(s.radius_nm - radius_nm) < 0.5f &&
           s.w == w && s.h == h && s.geo_cy == cy && s.bullseye_r == br;
}

const char *style_cache_tag(int style) {
    switch (style) {
    case MAP_BASEMAP_STYLE_DARK_NOLABELS:  return "darknl";
    case MAP_BASEMAP_STYLE_SECTIONAL:      return "vfrsec";
    case MAP_BASEMAP_STYLE_LIGHT:          return "voyager";
    case MAP_BASEMAP_STYLE_LIGHT_NOLABELS: return "voyagernl";
    case MAP_BASEMAP_STYLE_TOPO:           return "opentopo";
    case MAP_BASEMAP_STYLE_DARK:
    default:                               return "dark";
    }
}

// Days before a cached mosaic is considered stale and refetched.
int style_cache_ttl_days(int style) {
    switch (style) {
    case MAP_BASEMAP_STYLE_SECTIONAL:
        return 40;
    case MAP_BASEMAP_STYLE_TOPO:
        // Contours/landcover move slowly; same ballpark as OSM.
        return 30;
    case MAP_BASEMAP_STYLE_DARK:
    case MAP_BASEMAP_STYLE_DARK_NOLABELS:
    case MAP_BASEMAP_STYLE_LIGHT:
    case MAP_BASEMAP_STYLE_LIGHT_NOLABELS:
    default:
        return 30;
    }
}

// Empty/OOB fill matching each style's paper (avoids dark gutters on light maps).
void style_paper_rgb(int style, uint8_t &r, uint8_t &g, uint8_t &b) {
    switch (style) {
    case MAP_BASEMAP_STYLE_LIGHT:
    case MAP_BASEMAP_STYLE_LIGHT_NOLABELS:
        r = 0xfb; g = 0xf7; b = 0xf0; // Carto Voyager cream
        break;
    case MAP_BASEMAP_STYLE_TOPO:
        r = 0xe8; g = 0xe4; b = 0xd0;
        break;
    case MAP_BASEMAP_STYLE_SECTIONAL:
        r = 0xf2; g = 0xea; b = 0xd0; // chart paper
        break;
    case MAP_BASEMAP_STYLE_DARK:
    case MAP_BASEMAP_STYLE_DARK_NOLABELS:
    default:
        r = 0x0a; g = 0x0a; b = 0x1a;
        break;
    }
}

struct CurlBuf {
    std::vector<uint8_t> data;
};

size_t curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *b = static_cast<CurlBuf *>(userdata);
    size_t n = size * nmemb;
    b->data.insert(b->data.end(), ptr, ptr + n);
    return n;
}

bool http_get(const std::string &url, std::vector<uint8_t> &out) {
    static std::once_flag curl_once;
    std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) return false;
    CurlBuf buf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FlightLevel314-Basemap/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip if offered
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || (code != 200 && code != 404)) return false;
    out.swap(buf.data);
    return true;
}

// Parallel tile downloads (sequential curl was the dominant cost - ~100-140
// tiles at 50nm). Up to MAX_PARALLEL in flight; connection reuse via multi.
constexpr int MAX_PARALLEL = 8;

struct TileFetch {
    std::string url;
    int dst_x = 0;
    int dst_y = 0;
    CurlBuf buf;
    CURL *easy = nullptr;
    bool finished = false;
    bool ok = false;
    int attempts = 0; // includes the in-flight try
};

bool decode_tile_image(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &rgba,
                       unsigned &w, unsigned &h);
void blit_tile_rgba(std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                    const std::vector<uint8_t> &tile_rgba, int dst_x, int dst_y);

void bind_easy(TileFetch &job) {
    job.easy = curl_easy_init();
    job.buf.data.clear();
    job.finished = false;
    job.ok = false;
    curl_easy_setopt(job.easy, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(job.easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(job.easy, CURLOPT_WRITEDATA, &job.buf);
    curl_easy_setopt(job.easy, CURLOPT_USERAGENT, "FlightLevel314-Basemap/1.0");
    curl_easy_setopt(job.easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(job.easy, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(job.easy, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(job.easy, CURLOPT_ACCEPT_ENCODING, "");
    // OpenTopoMap drops multiplexed HTTP/2 streams under parallel load
    // (CURLE_HTTP2_STREAM / empty body) → missing squares. HTTP/1.1 is
    // reliable for all our tile hosts.
    curl_easy_setopt(job.easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(job.easy, CURLOPT_PRIVATE, &job);
}

// Fetch all jobs with curl_multi; decode+blit as each completes.
// Failed tiles are retried a couple of times (OpenTopo was especially flaky
// under HTTP/2 multiplex). Returns false if the request gen was superseded.
bool fetch_tiles_parallel(std::vector<TileFetch> &jobs,
                          std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                          uint32_t gen, int fetch_pct_end) {
    if (jobs.empty()) return true;
    static std::once_flag curl_once;
    std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURLM *multi = curl_multi_init();
    if (!multi) return false;
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)MAX_PARALLEL);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)MAX_PARALLEL);

    const int total = (int)jobs.size();
    int next = 0;
    int done = 0;
    int running = 0;
    int failed = 0;
    constexpr int MAX_ATTEMPTS = 3;

    auto start_more = [&]() {
        while (running < MAX_PARALLEL && next < total) {
            TileFetch &job = jobs[(size_t)next++];
            job.attempts = 1;
            bind_easy(job);
            curl_multi_add_handle(multi, job.easy);
            running++;
        }
    };

    start_more();
    progress_set(gen, true, 1);

    while (done < total) {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (gen != g_req_gen) {
                for (auto &job : jobs) {
                    if (job.easy) {
                        curl_multi_remove_handle(multi, job.easy);
                        curl_easy_cleanup(job.easy);
                        job.easy = nullptr;
                    }
                }
                curl_multi_cleanup(multi);
                return false;
            }
        }

        int still = 0;
        curl_multi_perform(multi, &still);

        int msgs = 0;
        while (CURLMsg *msg = curl_multi_info_read(multi, &msgs)) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL *easy = msg->easy_handle;
            char *priv = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &priv);
            TileFetch *job = reinterpret_cast<TileFetch *>(priv);
            long code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
            bool hop = (msg->data.result == CURLE_OK && code == 200 && job &&
                        !job->buf.data.empty());
            bool decoded = false;
            if (hop) {
                std::vector<uint8_t> rgba;
                unsigned tw = 0, th = 0;
                if (decode_tile_image(job->buf.data, rgba, tw, th)) {
                    blit_tile_rgba(mosaic, mosaic_w, mosaic_h, rgba,
                                   job->dst_x, job->dst_y);
                    decoded = true;
                }
            }

            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
            if (job) job->easy = nullptr;
            running--;

            if (job && !decoded && job->attempts < MAX_ATTEMPTS) {
                // Retry transient empty/HTTP2/timeout failures.
                job->attempts++;
                bind_easy(*job);
                curl_multi_add_handle(multi, job->easy);
                running++;
                continue;
            }

            if (job) {
                job->finished = true;
                job->ok = decoded;
                if (!decoded) failed++;
                job->buf.data.clear();
                job->buf.data.shrink_to_fit();
            }
            done++;
            progress_set(gen, true, (done * fetch_pct_end) / total);
            start_more();
        }

        if (done >= total) break;
        if (still == 0 && next >= total && running == 0) {
            break;
        }
        int wait_ms = 100;
        curl_multi_wait(multi, nullptr, 0, wait_ms, nullptr);
    }

    curl_multi_cleanup(multi);
    if (failed > 0) {
        platform_log("Basemap: %d/%d tiles failed after retries\n", failed, total);
    }
    return true;
}

int osm_zoom_for_radius(float radius_nm, int usable_h, float center_lat) {
    float our_ppd = (float)usable_h / (radius_nm * 2.0f) * 60.0f;
    float cos_lat = cosf(center_lat * (float)M_PI / 180.0f);
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
    // If the closest level is still softer than the screen, step up one so
    // wide ranges aren't intentionally undersampled (looks "a little soft").
    float best_ppd = 256.0f * (float)(1 << best_z) / 360.0f * cos_lat;
    if (best_ppd < our_ppd && best_z < 15) best_z++;
    return best_z;
}

int zoom_for_style(int style, float radius_nm, int usable_h, float center_lat) {
    int z = osm_zoom_for_radius(radius_nm, usable_h, center_lat);
    if (style == MAP_BASEMAP_STYLE_SECTIONAL) {
        if (z < SECTIONAL_Z_MIN) z = SECTIONAL_Z_MIN;
        if (z > SECTIONAL_Z_MAX) z = SECTIONAL_Z_MAX;
    }
    return z;
}

void format_tile_url(char *buf, size_t buflen, int style, int z, int x, int y) {
    switch (style) {
    case MAP_BASEMAP_STYLE_DARK_NOLABELS:
        snprintf(buf, buflen,
                 "https://basemaps.cartocdn.com/dark_nolabels/%d/%d/%d.png", z, x, y);
        break;
    case MAP_BASEMAP_STYLE_LIGHT:
        // Voyager is cream/warm — Carto light_all is near-white and harsh on a
        // night-cockpit display. Positron is no longer on this CDN.
        snprintf(buf, buflen,
                 "https://basemaps.cartocdn.com/rastertiles/voyager/%d/%d/%d.png", z, x, y);
        break;
    case MAP_BASEMAP_STYLE_LIGHT_NOLABELS:
        snprintf(buf, buflen,
                 "https://basemaps.cartocdn.com/rastertiles/voyager_nolabels/%d/%d/%d.png",
                 z, x, y);
        break;
    case MAP_BASEMAP_STYLE_TOPO:
        snprintf(buf, buflen,
                 "https://tile.opentopomap.org/%d/%d/%d.png", z, x, y);
        break;
    case MAP_BASEMAP_STYLE_SECTIONAL:
        // ArcGIS MapServer tile path is /tile/{z}/{y}/{x} (y before x).
        snprintf(buf, buflen,
                 "https://tiles.arcgis.com/tiles/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
                 "VFR_Sectional/MapServer/tile/%d/%d/%d",
                 z, y, x);
        break;
    case MAP_BASEMAP_STYLE_DARK:
    default:
        snprintf(buf, buflen,
                 "https://basemaps.cartocdn.com/dark_all/%d/%d/%d.png", z, x, y);
        break;
    }
}

void pixel_of_coord(float lat, float lon, int z, double &px, double &py) {
    int n = 1 << z;
    px = (lon + 180.0) / 360.0 * n * TILE_PX;
    double lat_rad = lat * M_PI / 180.0;
    py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * TILE_PX;
}

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

std::string cache_dir() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg && xdg[0] ? xdg : "";
    if (base.empty()) {
        const char *home = getenv("HOME");
        base = home && home[0] ? std::string(home) + "/.config" : "/tmp";
    }
    return base + "/flightlevel314/basemap";
}

void ensure_dir(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur.size() > 1 && cur.back() == '/') {
                std::string d = cur.substr(0, cur.size() - 1);
                mkdir(d.c_str(), 0755);
            } else if (i + 1 == path.size()) {
                mkdir(cur.c_str(), 0755);
            }
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string cache_path(int style, float lat, float lon, float radius_nm,
                       int w, int h, int cy, int br) {
    char name[220];
    snprintf(name, sizeof(name), "%s/%s_eq5_%.4f_%.4f_r%.0f_%dx%d_cy%d_br%d.rgb565",
             cache_dir().c_str(), style_cache_tag(style),
             lat, lon, radius_nm, w, h, cy, br);
    return name;
}

bool cache_fresh(const std::string &path, int ttl_days) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    time_t now = time(nullptr);
    if (now < st.st_mtime) return true; // clock skew — treat as fresh
    const time_t age = now - st.st_mtime;
    return age < (time_t)ttl_days * 86400;
}

bool load_cache(const std::string &path, BasemapSlot &slot) {
    if (!cache_fresh(path, style_cache_ttl_days(slot.style))) {
        platform_log("Basemap: cache expired %s (ttl=%dd)\n",
                     path.c_str(), style_cache_ttl_days(slot.style));
        return false;
    }
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz != slot.rgb565.size()) {
        fclose(f);
        return false;
    }
    size_t n = fread(slot.rgb565.data(), 1, slot.rgb565.size(), f);
    fclose(f);
    return n == slot.rgb565.size();
}

void save_cache(const std::string &path, const BasemapSlot &slot) {
    ensure_dir(cache_dir());
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(slot.rgb565.data(), 1, slot.rgb565.size(), f);
    fclose(f);
}

bool decode_tile_image(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &rgba,
                       unsigned &tw, unsigned &th) {
    int w = 0, h = 0, comp = 0;
    unsigned char *out = stbi_load_from_memory(bytes.data(), (int)bytes.size(),
                                               &w, &h, &comp, 4);
    if (!out) return false;
    tw = (unsigned)w;
    th = (unsigned)h;
    if (tw != (unsigned)TILE_PX || th != (unsigned)TILE_PX) {
        stbi_image_free(out);
        return false;
    }
    rgba.assign(out, out + (size_t)tw * th * 4);
    stbi_image_free(out);
    return true;
}

void blit_tile_rgba(std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                    const std::vector<uint8_t> &rgba, int dst_x0, int dst_y0) {
    for (int ty = 0; ty < TILE_PX; ty++) {
        int dy = dst_y0 + ty;
        if (dy < 0 || dy >= mosaic_h) continue;
        for (int tx = 0; tx < TILE_PX; tx++) {
            int dx = dst_x0 + tx;
            if (dx < 0 || dx >= mosaic_w) continue;
            const uint8_t *p = &rgba[((size_t)ty * TILE_PX + tx) * 4];
            uint8_t *d = &mosaic[((size_t)dy * mosaic_w + dx) * 4];
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3];
        }
    }
}

// Soften JPEG / engraved-chart scan structure before warping. Without this,
// Mercator→equirectangular resampling aliases fine horizontal texture into
// thick "corduroy" bands in flat chart paper / dark ocean fill.
void box_blur_3x3(std::vector<uint8_t> &img, int w, int h) {
    if (w < 3 || h < 3) return;
    std::vector<uint8_t> src = img;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int rs = 0, gs = 0, bs = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const uint8_t *p = &src[((size_t)(y + dy) * w + (x + dx)) * 4];
                    rs += p[0];
                    gs += p[1];
                    bs += p[2];
                }
            }
            uint8_t *d = &img[((size_t)y * w + x) * 4];
            d[0] = (uint8_t)(rs / 9);
            d[1] = (uint8_t)(gs / 9);
            d[2] = (uint8_t)(bs / 9);
        }
    }
}

// Bilinear sample of an RGBA mosaic. Out of bounds → style paper color.
void sample_mosaic_rgb(const std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                       double u, double v, uint8_t pr, uint8_t pg, uint8_t pb,
                       uint8_t &r, uint8_t &g, uint8_t &b) {
    if (u < 0 || v < 0 || u >= mosaic_w - 1 || v >= mosaic_h - 1) {
        r = pr; g = pg; b = pb;
        return;
    }
    int x0 = (int)floor(u);
    int y0 = (int)floor(v);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    double fx = u - x0;
    double fy = v - y0;
    auto at = [&](int x, int y) -> const uint8_t * {
        return &mosaic[((size_t)y * mosaic_w + x) * 4];
    };
    const uint8_t *p00 = at(x0, y0);
    const uint8_t *p10 = at(x1, y0);
    const uint8_t *p01 = at(x0, y1);
    const uint8_t *p11 = at(x1, y1);
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    r = (uint8_t)(lerp(lerp(p00[0], p10[0], fx), lerp(p01[0], p11[0], fx), fy) + 0.5);
    g = (uint8_t)(lerp(lerp(p00[1], p10[1], fx), lerp(p01[1], p11[1], fx), fy) + 0.5);
    b = (uint8_t)(lerp(lerp(p00[2], p10[2], fx), lerp(p01[2], p11[2], fx), fy) + 0.5);
}

// Average several bilinear taps over the screen-pixel footprint in mosaic space
// (cheap area filter — kills moiré when upsampling chart paper).
void sample_mosaic_area(const std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                        double u, double v, double du, double dv,
                        uint8_t pr, uint8_t pg, uint8_t pb,
                        uint8_t &r, uint8_t &g, uint8_t &b) {
    double span = du > dv ? du : dv;
    int n = 1;
    if (span > 1.25) n = 2;
    if (span > 2.5) n = 3;
    if (n == 1) {
        sample_mosaic_rgb(mosaic, mosaic_w, mosaic_h, u, v, pr, pg, pb, r, g, b);
        return;
    }
    double rs = 0, gs = 0, bs = 0;
    double u0 = u - 0.5 * du;
    double v0 = v - 0.5 * dv;
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            double uu = u0 + (i + 0.5) * du / n;
            double vv = v0 + (j + 0.5) * dv / n;
            uint8_t rr, gg, bb;
            sample_mosaic_rgb(mosaic, mosaic_w, mosaic_h, uu, vv, pr, pg, pb, rr, gg, bb);
            rs += rr; gs += gg; bs += bb;
        }
    }
    double inv = 1.0 / (n * n);
    r = (uint8_t)(rs * inv + 0.5);
    g = (uint8_t)(gs * inv + 0.5);
    b = (uint8_t)(bs * inv + 0.5);
}

// Build a basemap warped into MapProjection's local equirectangular frame so
// runways/aircraft (to_screen) land on the same geography as the tiles.
// Mercator tiles are fetched into a mosaic, then each canvas pixel is inverse-
// projected to lat/lon and sampled — fixes the center-only alignment of a
// 1:1 mercator blit.
bool build_basemap(BasemapSlot &slot, uint32_t gen) {
    const int usable_h = slot.bullseye_r * 2;
    if (usable_h <= 0 || slot.w <= 0 || slot.h <= 0 || slot.radius_nm <= 0)
        return false;

    // Matches MapProjection::to_screen with screen_h=geo_cy+R, top_margin=geo_cy-R.
    const float scale = (float)slot.bullseye_r / slot.radius_nm;
    const float cos_lat = cosf(slot.lat * (float)M_PI / 180.0f);
    if (scale <= 0.0f || fabsf(cos_lat) < 1e-4f) return false;

    auto screen_to_ll = [&](float sx, float sy, float &lat, float &lon) {
        float dx_nm = (sx - slot.w * 0.5f) / scale;
        float dy_nm = (slot.geo_cy - sy) / scale;
        lat = slot.lat + dy_nm / 60.0f;
        lon = slot.lon + dx_nm / (60.0f * cos_lat);
    };

    int z = zoom_for_style(slot.style, slot.radius_nm, usable_h, slot.lat);
    int n = 1 << z;

    // Mercator AABB covering the equirectangular canvas (corners + edge mids).
    double min_mx = 1e300, min_my = 1e300, max_mx = -1e300, max_my = -1e300;
    const float xs[] = {0.f, (float)(slot.w - 1), (float)(slot.w / 2)};
    const float ys[] = {0.f, (float)(slot.h - 1), (float)(slot.h / 2)};
    for (float sy : ys) {
        for (float sx : xs) {
            float lat, lon;
            screen_to_ll(sx, sy, lat, lon);
            if (lat > 85.0f) lat = 85.0f;
            if (lat < -85.0f) lat = -85.0f;
            double mx, my;
            pixel_of_coord(lat, lon, z, mx, my);
            if (mx < min_mx) min_mx = mx;
            if (my < min_my) min_my = my;
            if (mx > max_mx) max_mx = mx;
            if (my > max_my) max_my = my;
        }
    }

    int tx0 = (int)floor(min_mx / TILE_PX) - 1;
    int tx1 = (int)floor(max_mx / TILE_PX) + 1;
    int ty0 = (int)floor(min_my / TILE_PX) - 1;
    int ty1 = (int)floor(max_my / TILE_PX) + 1;
    // Clamp Y to valid mercator tile rows; X wraps.
    if (ty0 < 0) ty0 = 0;
    if (ty1 >= n) ty1 = n - 1;
    if (ty1 < ty0) return false;

    int tiles_w = tx1 - tx0 + 1;
    int tiles_h = ty1 - ty0 + 1;
    // Guard against pathological zoom/AABB (keeps worker memory bounded).
    if (tiles_w <= 0 || tiles_h <= 0 || tiles_w * tiles_h > 300) {
        platform_log("Basemap: tile AABB too large (%dx%d at z=%d), abort\n",
                     tiles_w, tiles_h, z);
        return false;
    }

    const int mosaic_w = tiles_w * TILE_PX;
    const int mosaic_h = tiles_h * TILE_PX;
    uint8_t paper_r, paper_g, paper_b;
    style_paper_rgb(slot.style, paper_r, paper_g, paper_b);
    std::vector<uint8_t> mosaic((size_t)mosaic_w * mosaic_h * 4);
    for (size_t i = 0; i < mosaic.size(); i += 4) {
        mosaic[i] = paper_r;
        mosaic[i + 1] = paper_g;
        mosaic[i + 2] = paper_b;
        mosaic[i + 3] = 255;
    }

    const int tile_total = tiles_w * tiles_h;
    // Fetch dominates; leave headroom so blur/warp keep the % counter moving
    // (it used to sit on 80% through the whole post-fetch phase).
    constexpr int FETCH_PCT_END = 70;
    constexpr int BLUR_PCT_END = 82;

    std::vector<TileFetch> jobs;
    jobs.reserve((size_t)tile_total);
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            int wtx = tx % n;
            if (wtx < 0) wtx += n;
            TileFetch job;
            char url[320];
            format_tile_url(url, sizeof(url), slot.style, z, wtx, ty);
            job.url = url;
            job.dst_x = (tx - tx0) * TILE_PX;
            job.dst_y = (ty - ty0) * TILE_PX;
            jobs.push_back(std::move(job));
        }
    }

    platform_log("Basemap: fetching %d tiles at z=%d (parallel %d)\n",
                 tile_total, z, MAX_PARALLEL);
    const uint32_t t_fetch0 = platform_millis();
    if (!fetch_tiles_parallel(jobs, mosaic, mosaic_w, mosaic_h, gen, FETCH_PCT_END))
        return false;
    platform_log("Basemap: tile fetch %ums for %d tiles\n",
                 (unsigned)(platform_millis() - t_fetch0), tile_total);

    // Anti-alias before the nonlinear warp. Critical for two reasons:
    // 1) Mercator tile *row* boundaries are constant-latitude → exact
    //    horizontal lines in MapProjection; hard seams read as thick
    //    scanlines across flat chart paper / dark ocean fill.
    // 2) JPEG / engraved-chart texture aliases into corduroy without a
    //    low-pass before resampling.
    // Keep this light — three/four full-mosaic passes were making wide
    // ranges look soft even with screen-matched zoom.
    const int blur_passes = (slot.style == MAP_BASEMAP_STYLE_SECTIONAL) ? 2 : 1;
    for (int i = 0; i < blur_passes; i++) {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (gen != g_req_gen) return false;
        }
        box_blur_3x3(mosaic, mosaic_w, mosaic_h);
        progress_set(gen, true,
                     FETCH_PCT_END + ((i + 1) * (BLUR_PCT_END - FETCH_PCT_END)) / blur_passes);
    }

    const double origin_mx = (double)tx0 * TILE_PX;
    const double origin_my = (double)ty0 * TILE_PX;

    uint16_t bg = rgb888_to_rgb565(paper_r, paper_g, paper_b);
    for (size_t i = 0; i < slot.rgb565.size(); i += 2) {
        slot.rgb565[i] = (uint8_t)(bg & 0xFF);
        slot.rgb565[i + 1] = (uint8_t)(bg >> 8);
    }

    const uint32_t t_warp0 = platform_millis();
    for (int sy = 0; sy < slot.h; sy++) {
        if ((sy & 15) == 0) {
            std::lock_guard<std::mutex> lock(g_mu);
            if (gen != g_req_gen) return false;
            // Warp phase: BLUR_PCT_END → 99%.
            g_prog_visible = true;
            g_prog_pct = BLUR_PCT_END + (sy * (99 - BLUR_PCT_END)) / slot.h;
        }
        for (int sx = 0; sx < slot.w; sx++) {
            float lat, lon, lat_r, lon_r, lat_d, lon_d;
            screen_to_ll(sx + 0.5f, sy + 0.5f, lat, lon);
            screen_to_ll(sx + 1.5f, sy + 0.5f, lat_r, lon_r);
            screen_to_ll(sx + 0.5f, sy + 1.5f, lat_d, lon_d);
            if (lat > 85.0f) lat = 85.0f;
            if (lat < -85.0f) lat = -85.0f;
            if (lat_r > 85.0f) lat_r = 85.0f;
            if (lat_r < -85.0f) lat_r = -85.0f;
            if (lat_d > 85.0f) lat_d = 85.0f;
            if (lat_d < -85.0f) lat_d = -85.0f;
            double mx, my, mx_r, my_r, mx_d, my_d;
            pixel_of_coord(lat, lon, z, mx, my);
            pixel_of_coord(lat_r, lon_r, z, mx_r, my_r);
            pixel_of_coord(lat_d, lon_d, z, mx_d, my_d);
            double du = fabs(mx_r - mx);
            double dv = fabs(my_d - my);
            // Do not floor to 1.0 — when the mosaic is sharper than the screen
            // (du/dv < 1) expanding the footprint only softens the result.
            if (du < 1e-6) du = 1.0;
            if (dv < 1e-6) dv = 1.0;
            uint8_t r, g, b;
            sample_mosaic_area(mosaic, mosaic_w, mosaic_h,
                               mx - origin_mx, my - origin_my, du, dv,
                               paper_r, paper_g, paper_b, r, g, b);
            uint16_t pix = rgb888_to_rgb565(r, g, b);
            size_t off = ((size_t)sy * slot.w + sx) * 2;
            slot.rgb565[off] = (uint8_t)(pix & 0xFF);
            slot.rgb565[off + 1] = (uint8_t)(pix >> 8);
        }
    }
    platform_log("Basemap: warp %ums (%dx%d)\n",
                 (unsigned)(platform_millis() - t_warp0), slot.w, slot.h);

    slot.bind_dsc();
    slot.valid = true;
    return true;
}

void worker_main(uint32_t gen) {
    BasemapSlot local;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        local.lat = g_req_lat;
        local.lon = g_req_lon;
        local.radius_nm = g_req_radius;
        local.w = g_req_w;
        local.h = g_req_h;
        local.geo_cy = g_req_cy;
        local.bullseye_r = g_req_br;
        local.style = g_req_style;
    }
    local.rgb565.assign((size_t)local.w * local.h * 2, 0);

    std::string path = cache_path(local.style, local.lat, local.lon, local.radius_nm,
                                  local.w, local.h, local.geo_cy, local.bullseye_r);
    bool ok = false;
    if (load_cache(path, local)) {
        local.bind_dsc();
        local.valid = true;
        ok = true;
        // Cache hit: don't flash the "Updating map..." chrome.
        progress_clear_if_gen(gen);
        platform_log("Basemap: cache hit %s\n", path.c_str());
    } else {
        progress_set(gen, true, 1);
        platform_log("Basemap: fetching style=%s (%.4f,%.4f) r=%.0fnm %dx%d\n",
                     style_cache_tag(local.style),
                     local.lat, local.lon, local.radius_nm, local.w, local.h);
        ok = build_basemap(local, gen);
        if (ok) {
            progress_set(gen, true, 100);
            save_cache(path, local);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (ok && gen == g_req_gen) {
            g_inbox = std::move(local);
            g_inbox.bind_dsc();
            g_inbox_ready = true;
            g_prog_visible = false;
            g_prog_pct = 0;
        } else if (gen == g_req_gen) {
            // Failed or cancelled for the current request — hide the bar.
            g_prog_visible = false;
            g_prog_pct = 0;
        }
        g_worker_busy = false;
    }
}

} // namespace

void basemap_request(float lat, float lon, float radius_nm, int canvas_w, int canvas_h,
                     int geo_center_y, int bullseye_r_px) {
    if (canvas_w <= 0 || canvas_h <= 0 || bullseye_r_px <= 0) return;

    const int style = map_basemap_style();

    std::lock_guard<std::mutex> lock(g_mu);
    const bool front_ok = slot_matches(g_front, lat, lon, radius_nm,
                                       canvas_w, canvas_h, geo_center_y, bullseye_r_px,
                                       style);
    if (front_ok && !g_worker_busy && !g_inbox_ready) {
        g_prog_visible = false;
        g_prog_pct = 0;
        return;
    }

    if (!front_ok) {
        g_front.valid = false;
    }
    g_inbox_ready = false;

    g_req_lat = lat;
    g_req_lon = lon;
    g_req_radius = radius_nm;
    g_req_w = canvas_w;
    g_req_h = canvas_h;
    g_req_cy = geo_center_y;
    g_req_br = bullseye_r_px;
    g_req_style = style;
    g_req_gen++;
    if (g_worker_busy) return;
    g_worker_busy = true;
    uint32_t gen = g_req_gen;
    std::thread([gen]() {
        worker_main(gen);
        uint32_t latest = 0;
        bool need = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            latest = g_req_gen;
            if (latest != gen && !g_worker_busy) {
                g_worker_busy = true;
                need = true;
            }
        }
        if (need) {
            std::thread(worker_main, latest).detach();
        }
    }).detach();
}

bool basemap_poll_swap(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_inbox_ready) return false;
    if (!slot_matches(g_inbox, g_req_lat, g_req_lon, g_req_radius,
                      g_req_w, g_req_h, g_req_cy, g_req_br, g_req_style)) {
        g_inbox_ready = false;
        return false;
    }
    g_front = std::move(g_inbox);
    g_front.bind_dsc();
    g_inbox_ready = false;
    return g_front.valid;
}

void basemap_draw(lv_layer_t *layer) {
    if (!map_basemap_shown()) return;
    if (!slot_matches(g_front, g_req_lat, g_req_lon, g_req_radius,
                      g_req_w, g_req_h, g_req_cy, g_req_br, g_req_style)) {
        return;
    }
    if (g_front.rgb565.empty()) return;

    g_front.bind_dsc();

    int pct = map_basemap_opa();
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = &g_front.dsc;
    img.opa = (lv_opa_t)((pct * 255) / 100);
    lv_area_t a = {0, 0, (lv_coord_t)(g_front.w - 1), (lv_coord_t)(g_front.h - 1)};
    lv_draw_image(layer, &img, &a);
}

bool basemap_ready(void) {
    return slot_matches(g_front, g_req_lat, g_req_lon, g_req_radius,
                        g_req_w, g_req_h, g_req_cy, g_req_br, g_req_style);
}

bool basemap_updating(int *out_pct) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (out_pct) *out_pct = g_prog_pct;
    return g_prog_visible;
}

int basemap_cache_clear(void) {
    // Drop in-memory mosaics first so draw doesn't keep showing deleted disk.
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_front = BasemapSlot{};
        g_inbox = BasemapSlot{};
        g_inbox_ready = false;
        g_req_gen++; // supersede any in-flight worker publish
        g_prog_visible = false;
        g_prog_pct = 0;
    }

    std::string dir = cache_dir();
    DIR *d = opendir(dir.c_str());
    if (!d) {
        platform_log("Basemap: cache clear — no dir at %s\n", dir.c_str());
        return 0;
    }
    int removed = 0;
    while (dirent *ent = readdir(d)) {
        if (!ent->d_name[0] || ent->d_name[0] == '.') continue;
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 8 || strcmp(name + len - 7, ".rgb565") != 0) continue;
        std::string path = dir + "/" + name;
        if (unlink(path.c_str()) == 0) removed++;
    }
    closedir(d);
    platform_log("Basemap: cache clear — removed %d file(s) from %s\n",
                 removed, dir.c_str());
    return removed;
}
