// Pi Map weather overlay: RainViewer precip radar (public API, no key),
// warped into MapProjection space on top of the basemap. Disk-cached
// ARGB8888 so clear sky stays transparent. Same inbox/poll threading
// model as basemap.cpp — never touch LVGL from the worker.

#include "weather.h"

#include "../src/platform/platform.h"
#include "../src/ui/display_prefs.h"

#include <ArduinoJson.h>
#include <curl/curl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>

// Decode only — stb_image implementation lives in basemap.cpp.
#define STBI_ONLY_PNG
#define STBI_NO_THREAD_LOCALS
#include "third_party/stb_image.h"

namespace {

constexpr int TILE_PX = 256;
constexpr int RV_Z_MAX = 7;       // RainViewer published max zoom
constexpr int RV_Z_MIN = 2;
constexpr int CACHE_TTL_SEC = 480; // ~8 min; frames refresh ~10 min
constexpr int MAX_PARALLEL = 6;
constexpr const char *MAPS_URL = "https://api.rainviewer.com/public/weather-maps.json";

struct WeatherSlot {
    std::vector<uint8_t> argb; // LVGL ARGB8888: B,G,R,A per pixel
    lv_image_dsc_t dsc{};
    float lat = 0, lon = 0, radius_nm = 0;
    int w = 0, h = 0;
    int geo_cy = 0;
    int bullseye_r = 0;
    time_t built_at = 0;
    bool valid = false;

    void bind_dsc() {
        memset(&dsc, 0, sizeof(dsc));
        dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        dsc.header.w = w;
        dsc.header.h = h;
        dsc.header.stride = w * 4;
        dsc.data_size = (uint32_t)argb.size();
        dsc.data = argb.data();
    }
};

std::mutex g_mu;
WeatherSlot g_front;
WeatherSlot g_inbox;
bool g_inbox_ready = false;
bool g_worker_busy = false;
float g_req_lat = 0, g_req_lon = 0, g_req_radius = 0;
int g_req_w = 0, g_req_h = 0, g_req_cy = 0, g_req_br = 0;
uint32_t g_req_gen = 0;

bool slot_matches(const WeatherSlot &s, float lat, float lon, float radius_nm,
                  int w, int h, int cy, int br) {
    return s.valid &&
           fabsf(s.lat - lat) < 1e-4f && fabsf(s.lon - lon) < 1e-4f &&
           fabsf(s.radius_nm - radius_nm) < 0.5f &&
           s.w == w && s.h == h && s.geo_cy == cy && s.bullseye_r == br;
}

bool slot_fresh(const WeatherSlot &s) {
    if (!s.valid || s.built_at <= 0) return false;
    time_t now = time(nullptr);
    if (now < s.built_at) return true;
    return (now - s.built_at) < CACHE_TTL_SEC;
}

struct CurlBuf {
    std::vector<uint8_t> data;
};

size_t curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<CurlBuf *>(userdata);
    size_t n = size * nmemb;
    buf->data.insert(buf->data.end(), ptr, ptr + n);
    return n;
}

bool http_get(const char *url, std::vector<uint8_t> &out, long timeout_s = 20) {
    out.clear();
    CURL *easy = curl_easy_init();
    if (!easy) return false;
    CurlBuf buf;
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "flightlevel314/1.0 (RainViewer overlay)");
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode rc = curl_easy_perform(easy);
    long code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK || code != 200 || buf.data.empty()) return false;
    out = std::move(buf.data);
    return true;
}

struct TileFetch {
    std::string url;
    int dst_x = 0, dst_y = 0;
    CurlBuf buf;
    CURL *easy = nullptr;
    int attempts = 0;
    bool finished = false;
    bool ok = false;
};

void bind_easy(TileFetch &job) {
    job.buf.data.clear();
    job.easy = curl_easy_init();
    curl_easy_setopt(job.easy, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(job.easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(job.easy, CURLOPT_WRITEDATA, &job.buf);
    curl_easy_setopt(job.easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(job.easy, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(job.easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(job.easy, CURLOPT_USERAGENT, "flightlevel314/1.0 (RainViewer overlay)");
    curl_easy_setopt(job.easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(job.easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(job.easy, CURLOPT_PRIVATE, &job);
}

bool decode_tile_png(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &rgba,
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

bool fetch_tiles_parallel(std::vector<TileFetch> &jobs,
                          std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                          uint32_t gen) {
    if (jobs.empty()) return true;
    static std::once_flag curl_once;
    std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURLM *multi = curl_multi_init();
    if (!multi) return false;
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)MAX_PARALLEL);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)MAX_PARALLEL);

    const int total = (int)jobs.size();
    int next = 0, done = 0, running = 0, failed = 0;
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
                if (decode_tile_png(job->buf.data, rgba, tw, th)) {
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
            start_more();
        }

        if (done >= total) break;
        if (still == 0 && next >= total && running == 0) break;
        curl_multi_wait(multi, nullptr, 0, 100, nullptr);
    }

    curl_multi_cleanup(multi);
    if (failed > 0) {
        platform_log("Weather: %d/%d tiles failed after retries\n", failed, total);
    }
    return true;
}

int rainviewer_zoom(float radius_nm, int usable_h, float center_lat) {
    float our_ppd = (float)usable_h / (radius_nm * 2.0f) * 60.0f;
    float cos_lat = cosf(center_lat * (float)M_PI / 180.0f);
    int best_z = RV_Z_MIN;
    float best_diff = 1e9f;
    for (int z = RV_Z_MIN; z <= RV_Z_MAX; z++) {
        float osm_ppd = 256.0f * (float)(1 << z) / 360.0f * cos_lat;
        float diff = fabsf(logf(osm_ppd / our_ppd));
        if (diff < best_diff) {
            best_diff = diff;
            best_z = z;
        }
    }
    return best_z;
}

void pixel_of_coord(float lat, float lon, int z, double &px, double &py) {
    int n = 1 << z;
    px = (lon + 180.0) / 360.0 * n * TILE_PX;
    double lat_rad = lat * M_PI / 180.0;
    py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * TILE_PX;
}

std::string cache_dir() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg && xdg[0] ? xdg : "";
    if (base.empty()) {
        const char *home = getenv("HOME");
        base = home && home[0] ? std::string(home) + "/.config" : "/tmp";
    }
    return base + "/flightlevel314/weather";
}

void ensure_dir(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur.size() > 1 && cur.back() == '/') {
                mkdir(cur.substr(0, cur.size() - 1).c_str(), 0755);
            } else if (i + 1 == path.size()) {
                mkdir(cur.c_str(), 0755);
            }
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string cache_path(float lat, float lon, float radius_nm,
                       int w, int h, int cy, int br) {
    char name[220];
    snprintf(name, sizeof(name), "%s/wx_eq1_%.4f_%.4f_r%.0f_%dx%d_cy%d_br%d.argb",
             cache_dir().c_str(), lat, lon, radius_nm, w, h, cy, br);
    return name;
}

bool cache_fresh(const std::string &path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    time_t now = time(nullptr);
    if (now < st.st_mtime) return true;
    return (now - st.st_mtime) < CACHE_TTL_SEC;
}

bool load_cache(const std::string &path, WeatherSlot &slot) {
    if (!cache_fresh(path)) return false;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz != slot.argb.size()) {
        fclose(f);
        return false;
    }
    size_t n = fread(slot.argb.data(), 1, slot.argb.size(), f);
    fclose(f);
    if (n != slot.argb.size()) return false;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) slot.built_at = st.st_mtime;
    else slot.built_at = time(nullptr);
    return true;
}

void save_cache(const std::string &path, const WeatherSlot &slot) {
    ensure_dir(cache_dir());
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(slot.argb.data(), 1, slot.argb.size(), f);
    fclose(f);
}

bool resolve_latest_frame(std::string &host, std::string &frame_path) {
    std::vector<uint8_t> body;
    if (!http_get(MAPS_URL, body, 15)) {
        platform_log("Weather: failed to fetch weather-maps.json\n");
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.data(), body.size());
    if (err) {
        platform_log("Weather: maps.json parse error: %s\n", err.c_str());
        return false;
    }
    const char *h = doc["host"];
    JsonArray past = doc["radar"]["past"].as<JsonArray>();
    if (!h || !h[0] || past.isNull() || past.size() == 0) {
        platform_log("Weather: maps.json missing host/past frames\n");
        return false;
    }
    JsonObject last = past[past.size() - 1];
    const char *p = last["path"];
    if (!p || !p[0]) return false;
    host = h;
    frame_path = p;
    return true;
}

// Bilinear RGBA sample; out of bounds → fully transparent.
void sample_mosaic_rgba(const std::vector<uint8_t> &mosaic, int mosaic_w, int mosaic_h,
                        double u, double v,
                        uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a) {
    if (u < 0 || v < 0 || u >= mosaic_w - 1 || v >= mosaic_h - 1) {
        r = g = b = a = 0;
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
    auto lerp = [](double x, double y, double t) { return x + (y - x) * t; };
    // Premultiply-ish lerp on straight alpha is fine for soft radar edges.
    r = (uint8_t)(lerp(lerp(p00[0], p10[0], fx), lerp(p01[0], p11[0], fx), fy) + 0.5);
    g = (uint8_t)(lerp(lerp(p00[1], p10[1], fx), lerp(p01[1], p11[1], fx), fy) + 0.5);
    b = (uint8_t)(lerp(lerp(p00[2], p10[2], fx), lerp(p01[2], p11[2], fx), fy) + 0.5);
    a = (uint8_t)(lerp(lerp(p00[3], p10[3], fx), lerp(p01[3], p11[3], fx), fy) + 0.5);
}

bool build_weather(WeatherSlot &slot, uint32_t gen) {
    const int usable_h = slot.bullseye_r * 2;
    if (usable_h <= 0 || slot.w <= 0 || slot.h <= 0 || slot.radius_nm <= 0)
        return false;

    const float scale = (float)slot.bullseye_r / slot.radius_nm;
    const float cos_lat = cosf(slot.lat * (float)M_PI / 180.0f);
    if (scale <= 0.0f || fabsf(cos_lat) < 1e-4f) return false;

    auto screen_to_ll = [&](float sx, float sy, float &lat, float &lon) {
        float dx_nm = (sx - slot.w * 0.5f) / scale;
        float dy_nm = (slot.geo_cy - sy) / scale;
        lat = slot.lat + dy_nm / 60.0f;
        lon = slot.lon + dx_nm / (60.0f * cos_lat);
    };

    std::string host, frame_path;
    if (!resolve_latest_frame(host, frame_path)) return false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (gen != g_req_gen) return false;
    }

    int z = rainviewer_zoom(slot.radius_nm, usable_h, slot.lat);
    int n = 1 << z;

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
    if (ty0 < 0) ty0 = 0;
    if (ty1 >= n) ty1 = n - 1;
    if (ty1 < ty0) return false;

    int tiles_w = tx1 - tx0 + 1;
    int tiles_h = ty1 - ty0 + 1;
    // z≤7 keeps tile counts small; still guard memory.
    if (tiles_w <= 0 || tiles_h <= 0 || tiles_w * tiles_h > 200) {
        platform_log("Weather: tile AABB too large (%dx%d at z=%d), abort\n",
                     tiles_w, tiles_h, z);
        return false;
    }

    const int mosaic_w = tiles_w * TILE_PX;
    const int mosaic_h = tiles_h * TILE_PX;
    std::vector<uint8_t> mosaic((size_t)mosaic_w * mosaic_h * 4, 0);

    std::vector<TileFetch> jobs;
    jobs.reserve((size_t)tiles_w * tiles_h);
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            int wtx = tx % n;
            if (wtx < 0) wtx += n;
            TileFetch job;
            char url[420];
            // color=2 (universal), smooth=1, snow=1
            snprintf(url, sizeof(url), "%s%s/256/%d/%d/%d/2/1_1.png",
                     host.c_str(), frame_path.c_str(), z, wtx, ty);
            job.url = url;
            job.dst_x = (tx - tx0) * TILE_PX;
            job.dst_y = (ty - ty0) * TILE_PX;
            jobs.push_back(std::move(job));
        }
    }

    platform_log("Weather: fetching %d tiles at z=%d frame=%s\n",
                 (int)jobs.size(), z, frame_path.c_str());
    if (!fetch_tiles_parallel(jobs, mosaic, mosaic_w, mosaic_h, gen))
        return false;

    const double origin_mx = (double)tx0 * TILE_PX;
    const double origin_my = (double)ty0 * TILE_PX;

    // Transparent fill.
    std::fill(slot.argb.begin(), slot.argb.end(), 0);

    for (int sy = 0; sy < slot.h; sy++) {
        if ((sy & 31) == 0) {
            std::lock_guard<std::mutex> lock(g_mu);
            if (gen != g_req_gen) return false;
        }
        for (int sx = 0; sx < slot.w; sx++) {
            float lat, lon;
            screen_to_ll(sx + 0.5f, sy + 0.5f, lat, lon);
            if (lat > 85.0f) lat = 85.0f;
            if (lat < -85.0f) lat = -85.0f;
            double mx, my;
            pixel_of_coord(lat, lon, z, mx, my);
            uint8_t r, g, b, a;
            sample_mosaic_rgba(mosaic, mosaic_w, mosaic_h,
                               mx - origin_mx, my - origin_my, r, g, b, a);
            // LVGL ARGB8888 little-endian memory order: B,G,R,A
            size_t off = ((size_t)sy * slot.w + sx) * 4;
            slot.argb[off + 0] = b;
            slot.argb[off + 1] = g;
            slot.argb[off + 2] = r;
            slot.argb[off + 3] = a;
        }
    }

    slot.built_at = time(nullptr);
    slot.bind_dsc();
    slot.valid = true;
    return true;
}

void worker_main(uint32_t gen) {
    WeatherSlot local;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        local.lat = g_req_lat;
        local.lon = g_req_lon;
        local.radius_nm = g_req_radius;
        local.w = g_req_w;
        local.h = g_req_h;
        local.geo_cy = g_req_cy;
        local.bullseye_r = g_req_br;
    }
    local.argb.assign((size_t)local.w * local.h * 4, 0);

    std::string path = cache_path(local.lat, local.lon, local.radius_nm,
                                  local.w, local.h, local.geo_cy, local.bullseye_r);
    bool ok = false;
    if (load_cache(path, local)) {
        local.bind_dsc();
        local.valid = true;
        ok = true;
        platform_log("Weather: cache hit %s\n", path.c_str());
    } else {
        platform_log("Weather: building (%.4f,%.4f) r=%.0fnm %dx%d\n",
                     local.lat, local.lon, local.radius_nm, local.w, local.h);
        ok = build_weather(local, gen);
        if (ok) save_cache(path, local);
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (ok && gen == g_req_gen) {
            g_inbox = std::move(local);
            g_inbox.bind_dsc();
            g_inbox_ready = true;
            platform_log("Weather: ready %dx%d\n", g_inbox.w, g_inbox.h);
        } else if (gen != g_req_gen) {
            platform_log("Weather: discarded superseded build (gen %u → %u)\n",
                         (unsigned)gen, (unsigned)g_req_gen);
        }
        g_worker_busy = false;
    }
}

} // namespace

void weather_request(float lat, float lon, float radius_nm, int canvas_w, int canvas_h,
                     int geo_center_y, int bullseye_r_px) {
    if (!map_weather_shown()) return;
    if (canvas_w <= 0 || canvas_h <= 0 || bullseye_r_px <= 0) return;

    std::lock_guard<std::mutex> lock(g_mu);
    const bool front_ok = slot_matches(g_front, lat, lon, radius_nm,
                                       canvas_w, canvas_h, geo_center_y, bullseye_r_px);
    // Map's 1s timer calls us every tick for TTL refresh. A fresh front with
    // nothing pending is a no-op — do not bump g_req_gen.
    if (front_ok && slot_fresh(g_front) && !g_worker_busy && !g_inbox_ready) {
        return;
    }

    const bool same_req =
        fabsf(g_req_lat - lat) < 1e-4f && fabsf(g_req_lon - lon) < 1e-4f &&
        fabsf(g_req_radius - radius_nm) < 0.5f &&
        g_req_w == canvas_w && g_req_h == canvas_h &&
        g_req_cy == geo_center_y && g_req_br == bullseye_r_px &&
        g_req_w > 0;

    // Already fetching (or holding a finished inbox) for this geometry —
    // bumping gen here used to cancel the worker mid-fetch forever.
    if (same_req && (g_worker_busy || g_inbox_ready)) {
        return;
    }

    // Geometry changed: drop the drawn buffer. TTL refresh keeps the stale
    // front visible until the new inbox swaps in.
    if (!front_ok) {
        g_front.valid = false;
    }
    if (!same_req) {
        g_inbox_ready = false;
    }

    g_req_lat = lat;
    g_req_lon = lon;
    g_req_radius = radius_nm;
    g_req_w = canvas_w;
    g_req_h = canvas_h;
    g_req_cy = geo_center_y;
    g_req_br = bullseye_r_px;
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

bool weather_poll_swap(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_inbox_ready) return false;
    if (!slot_matches(g_inbox, g_req_lat, g_req_lon, g_req_radius,
                      g_req_w, g_req_h, g_req_cy, g_req_br)) {
        g_inbox_ready = false;
        return false;
    }
    g_front = std::move(g_inbox);
    g_front.bind_dsc();
    g_inbox_ready = false;
    return g_front.valid;
}

void weather_draw(lv_layer_t *layer) {
    if (!map_weather_shown()) return;
    if (!slot_matches(g_front, g_req_lat, g_req_lon, g_req_radius,
                      g_req_w, g_req_h, g_req_cy, g_req_br)) {
        return;
    }
    if (g_front.argb.empty()) return;

    g_front.bind_dsc();

    int pct = map_weather_opa();
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = &g_front.dsc;
    img.opa = (lv_opa_t)((pct * 255) / 100);
    lv_area_t a = {0, 0, (lv_coord_t)(g_front.w - 1), (lv_coord_t)(g_front.h - 1)};
    lv_draw_image(layer, &img, &a);
}

bool weather_ready(void) {
    return slot_matches(g_front, g_req_lat, g_req_lon, g_req_radius,
                        g_req_w, g_req_h, g_req_cy, g_req_br);
}

int weather_cache_clear(void) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_front = WeatherSlot{};
        g_inbox = WeatherSlot{};
        g_inbox_ready = false;
        g_req_gen++;
    }

    std::string dir = cache_dir();
    DIR *d = opendir(dir.c_str());
    if (!d) {
        platform_log("Weather: cache clear — no dir at %s\n", dir.c_str());
        return 0;
    }
    int removed = 0;
    while (dirent *ent = readdir(d)) {
        if (!ent->d_name[0] || ent->d_name[0] == '.') continue;
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".argb") != 0) continue;
        std::string path = dir + "/" + name;
        if (unlink(path.c_str()) == 0) removed++;
    }
    closedir(d);
    platform_log("Weather: cache clear — removed %d file(s) from %s\n",
                 removed, dir.c_str());
    return removed;
}
