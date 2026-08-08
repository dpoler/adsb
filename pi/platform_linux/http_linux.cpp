#include "../../src/platform/platform.h"
#include <curl/curl.h>
#include <string>
#include <cstring>
#include <mutex>

namespace {

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::once_flag curl_init_flag;

bool http_get_internal(const char *url, char *out, size_t out_size, size_t *out_len,
                       long *http_status, const char *const *extra_headers,
                       bool require_2xx) {
    std::call_once(curl_init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    // Planespotters (and some other APIs) reject generic library UAs with
    // HTTP 403 -- identify the app and include a contact URL.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "flightlevel314/0.1 (+https://github.com/dpoler/FlightLevel314)");

    struct curl_slist *hdrs = nullptr;
    if (extra_headers) {
        for (const char *const *h = extra_headers; *h; ++h) {
            hdrs = curl_slist_append(hdrs, *h);
        }
        if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        platform_log("HTTP GET %s failed: curl=%d\n", url, (int)res);
        return false;
    }
    if (http_status) *http_status = http_code;

    if (require_2xx && (http_code < 200 || http_code >= 300)) {
        platform_log("HTTP GET %s failed: http=%ld\n", url, http_code);
        return false;
    }

    size_t n = body.size();
    if (out_size > 0 && n >= out_size) n = out_size - 1;
    if (out_size > 0) {
        memcpy(out, body.data(), n);
        out[n] = '\0';
    }
    if (out_len) *out_len = n;
    return true;
}

} // namespace

bool platform_http_get(const char *url, char *out, size_t out_size, size_t *out_len) {
    return http_get_internal(url, out, out_size, out_len, nullptr, nullptr, true);
}

bool platform_http_get_ex(const char *url, char *out, size_t out_size, size_t *out_len,
                          long *http_status, const char *const *extra_headers) {
    return http_get_internal(url, out, out_size, out_len, http_status, extra_headers, false);
}
