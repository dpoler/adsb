// Linux config storage -- JSON file at ~/.config/flightlevel314/config.json
// (or $XDG_CONFIG_HOME/flightlevel314/config.json). Field names match the
// historical ESP32 NVS keys from dpoler/adsb for easy config migration.

#include "../../src/data/storage.h"
#include "../../src/platform/platform.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

UserConfig g_config = {};

static std::string config_dir() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return std::string(xdg) + "/flightlevel314";
    const char *home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/flightlevel314";
}

static std::string config_file_path() {
    return config_dir() + "/config.json";
}

static UserConfig defaults() {
    UserConfig cfg{};
    cfg.wifi_ssid[0] = '\0';
    cfg.wifi_pass[0] = '\0';
    cfg.airportdb_token[0] = '\0';
    cfg.airportdb_enabled = true; // preserve pre-toggle behavior for existing tokens
    cfg.aerodatabox_key[0] = '\0';
    cfg.aerodatabox_provider = 0; // RapidAPI
    cfg.aerodatabox_enabled = false;
    cfg.adbox_usage_yyyymm = 0;
    cfg.adbox_usage_count = 0;
    cfg.adbox_soft_limit = 0;
    cfg.adbox_rate_limited = false;
    cfg.radius_nm = 50;
    cfg.radius_presets[0] = 5;
    cfg.radius_presets[1] = 10;
    cfg.radius_presets[2] = 20;
    cfg.radius_presets[3] = 50;
    cfg.use_metric = false;
    cfg.use_ethernet = false;
    cfg.watchlist_count = 0;
    cfg.alert_military = true;
    cfg.alert_emergency = true;
    cfg.trail_style = 0;
    cfg.display_brightness_pct = 100;
    cfg.display_dim_after_min = 0;
    cfg.display_blank_after_min = 0;
    cfg.screensaver_enabled = false;
    cfg.screensaver_drift = true;
    for (int i = 0; i < 4; i++) {
        cfg.view_filter_mask[i] = 0;
        cfg.view_hide_ground[i] = false;
    }
    for (int i = 0; i < 2; i++) {
        cfg.view_trails_enabled[i] = true;
        cfg.view_trail_max_points[i] = 30;
        cfg.view_show_tag_id[i] = true;
        cfg.view_show_tag_data[i] = false;
        cfg.view_show_tag_type[i] = false;
        cfg.view_show_secondary_locations[i] = true;
    }
    cfg.map_basemap_enabled = true;
    for (int i = 0; i < 6; i++) cfg.map_basemap_opa[i] = 50;
    cfg.map_basemap_style = 0;
    cfg.map_weather_enabled = false;
    cfg.map_weather_opa = 60;
    cfg.last_view_idx = 0;
    cfg.last_range_idx = 0;
    cfg.last_location_name[0] = '\0';
    return cfg;
}

UserConfig storage_load_config() {
    UserConfig cfg = defaults();

    FILE *f = fopen(config_file_path().c_str(), "r");
    if (!f) {
        platform_log("Storage: no config file yet at %s, using defaults\n", config_file_path().c_str());
        return cfg;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) {
        fclose(f);
        return cfg;
    }
    std::string buf(size, '\0');
    size_t read = fread(&buf[0], 1, size, f);
    fclose(f);
    buf.resize(read);

    JsonDocument doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) {
        platform_log("Storage: %s failed to parse, using defaults\n", config_file_path().c_str());
        return cfg;
    }

    strlcpy(cfg.wifi_ssid, doc["ssid"] | cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, doc["pass"] | cfg.wifi_pass, sizeof(cfg.wifi_pass));
    strlcpy(cfg.airportdb_token, doc["apt_tok"] | cfg.airportdb_token, sizeof(cfg.airportdb_token));
    cfg.airportdb_enabled = doc["apt_en"] | cfg.airportdb_enabled;
    strlcpy(cfg.aerodatabox_key, doc["adbox_key"] | cfg.aerodatabox_key, sizeof(cfg.aerodatabox_key));
    cfg.aerodatabox_provider = doc["adbox_prov"] | cfg.aerodatabox_provider;
    if (cfg.aerodatabox_provider < 0 || cfg.aerodatabox_provider > 2) cfg.aerodatabox_provider = 0;
    cfg.aerodatabox_enabled = doc["adbox_en"] | cfg.aerodatabox_enabled;
    cfg.adbox_usage_yyyymm = doc["adbox_ym"] | cfg.adbox_usage_yyyymm;
    cfg.adbox_usage_count = doc["adbox_n"] | cfg.adbox_usage_count;
    cfg.adbox_soft_limit = doc["adbox_lim"] | cfg.adbox_soft_limit;
    cfg.adbox_rate_limited = doc["adbox_rl"] | cfg.adbox_rate_limited;
    cfg.radius_nm = doc["radius"] | cfg.radius_nm;
    cfg.radius_presets[0] = doc["rad0"] | cfg.radius_presets[0];
    cfg.radius_presets[1] = doc["rad1"] | cfg.radius_presets[1];
    cfg.radius_presets[2] = doc["rad2"] | cfg.radius_presets[2];
    cfg.radius_presets[3] = doc["rad3"] | cfg.radius_presets[3];
    cfg.use_metric = doc["metric"] | cfg.use_metric;
    cfg.use_ethernet = doc["use_eth"] | cfg.use_ethernet;
    cfg.alert_military = doc["alrt_mil"] | cfg.alert_military;
    cfg.alert_emergency = doc["alrt_emg"] | cfg.alert_emergency;
    cfg.trail_style = doc["trail_sty"] | cfg.trail_style;
    cfg.display_brightness_pct = doc["disp_bright"] | cfg.display_brightness_pct;
    cfg.display_dim_after_min = doc["disp_dimmin"] | cfg.display_dim_after_min;
    cfg.display_blank_after_min = doc["disp_blkmin"] | cfg.display_blank_after_min;
    cfg.screensaver_enabled = doc["ss_enabled"] | cfg.screensaver_enabled;
    cfg.screensaver_drift = doc["ss_drift"] | cfg.screensaver_drift;
    for (int i = 0; i < 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "filt_m%d", i);
        cfg.view_filter_mask[i] = doc[key] | cfg.view_filter_mask[i];
        snprintf(key, sizeof(key), "hide_gnd%d", i);
        cfg.view_hide_ground[i] = doc[key] | cfg.view_hide_ground[i];
    }
    cfg.view_trails_enabled[0] = doc["trail_on0"] | cfg.view_trails_enabled[0];
    cfg.view_trails_enabled[1] = doc["trail_on1"] | cfg.view_trails_enabled[1];
    cfg.view_trail_max_points[0] = doc["trail_pts0"] | cfg.view_trail_max_points[0];
    cfg.view_trail_max_points[1] = doc["trail_pts1"] | cfg.view_trail_max_points[1];
    cfg.view_show_tag_id[0] = doc["tag_id0"] | cfg.view_show_tag_id[0];
    cfg.view_show_tag_id[1] = doc["tag_id1"] | cfg.view_show_tag_id[1];
    cfg.view_show_tag_data[0] = doc["tag_data0"] | cfg.view_show_tag_data[0];
    cfg.view_show_tag_data[1] = doc["tag_data1"] | cfg.view_show_tag_data[1];
    cfg.view_show_tag_type[0] = doc["tag_type0"] | cfg.view_show_tag_type[0];
    cfg.view_show_tag_type[1] = doc["tag_type1"] | cfg.view_show_tag_type[1];
    cfg.view_show_secondary_locations[0] = doc["show2loc0"] | cfg.view_show_secondary_locations[0];
    cfg.view_show_secondary_locations[1] = doc["show2loc1"] | cfg.view_show_secondary_locations[1];
    cfg.map_basemap_enabled = doc["bm_on"] | cfg.map_basemap_enabled;
    // Legacy single bm_opa seeds all styles if per-style keys are absent.
    int legacy_opa = doc["bm_opa"] | 50;
    if (legacy_opa < 10) legacy_opa = 10;
    if (legacy_opa > 100) legacy_opa = 100;
    for (int i = 0; i < 6; i++) {
        char key[12];
        snprintf(key, sizeof(key), "bm_opa%d", i);
        cfg.map_basemap_opa[i] = doc[key] | legacy_opa;
        if (cfg.map_basemap_opa[i] < 10) cfg.map_basemap_opa[i] = 10;
        if (cfg.map_basemap_opa[i] > 100) cfg.map_basemap_opa[i] = 100;
    }
    cfg.map_basemap_style = doc["bm_style"] | cfg.map_basemap_style;
    if (cfg.map_basemap_style < 0) cfg.map_basemap_style = 0;
    if (cfg.map_basemap_style > 5) cfg.map_basemap_style = 5;
    cfg.map_weather_enabled = doc["wx_on"] | cfg.map_weather_enabled;
    cfg.map_weather_opa = doc["wx_opa"] | cfg.map_weather_opa;
    if (cfg.map_weather_opa < 10) cfg.map_weather_opa = 10;
    if (cfg.map_weather_opa > 100) cfg.map_weather_opa = 100;
    cfg.last_view_idx = doc["last_view"] | cfg.last_view_idx;
    cfg.last_range_idx = doc["last_rng"] | cfg.last_range_idx;
    strlcpy(cfg.last_location_name, doc["last_loc"] | cfg.last_location_name, sizeof(cfg.last_location_name));

    platform_log("Storage: config loaded from %s\n", config_file_path().c_str());
    return cfg;
}

void storage_save_config(const UserConfig &cfg) {
    mkdir(config_dir().c_str(), 0755); // ignores EEXIST -- fine either way

    JsonDocument doc;
    doc["ssid"] = cfg.wifi_ssid;
    doc["pass"] = cfg.wifi_pass;
    doc["apt_tok"] = cfg.airportdb_token;
    doc["apt_en"] = cfg.airportdb_enabled;
    doc["adbox_key"] = cfg.aerodatabox_key;
    doc["adbox_prov"] = cfg.aerodatabox_provider;
    doc["adbox_en"] = cfg.aerodatabox_enabled;
    doc["adbox_ym"] = cfg.adbox_usage_yyyymm;
    doc["adbox_n"] = cfg.adbox_usage_count;
    doc["adbox_lim"] = cfg.adbox_soft_limit;
    doc["adbox_rl"] = cfg.adbox_rate_limited;
    doc["radius"] = cfg.radius_nm;
    doc["rad0"] = cfg.radius_presets[0];
    doc["rad1"] = cfg.radius_presets[1];
    doc["rad2"] = cfg.radius_presets[2];
    doc["rad3"] = cfg.radius_presets[3];
    doc["metric"] = cfg.use_metric;
    doc["use_eth"] = cfg.use_ethernet;
    doc["alrt_mil"] = cfg.alert_military;
    doc["alrt_emg"] = cfg.alert_emergency;
    doc["trail_sty"] = cfg.trail_style;
    doc["disp_bright"] = cfg.display_brightness_pct;
    doc["disp_dimmin"] = cfg.display_dim_after_min;
    doc["disp_blkmin"] = cfg.display_blank_after_min;
    doc["ss_enabled"] = cfg.screensaver_enabled;
    doc["ss_drift"] = cfg.screensaver_drift;
    for (int i = 0; i < 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "filt_m%d", i);
        doc[key] = cfg.view_filter_mask[i];
        snprintf(key, sizeof(key), "hide_gnd%d", i);
        doc[key] = cfg.view_hide_ground[i];
    }
    doc["trail_on0"] = cfg.view_trails_enabled[0];
    doc["trail_on1"] = cfg.view_trails_enabled[1];
    doc["trail_pts0"] = cfg.view_trail_max_points[0];
    doc["trail_pts1"] = cfg.view_trail_max_points[1];
    doc["tag_id0"] = cfg.view_show_tag_id[0];
    doc["tag_id1"] = cfg.view_show_tag_id[1];
    doc["tag_data0"] = cfg.view_show_tag_data[0];
    doc["tag_data1"] = cfg.view_show_tag_data[1];
    doc["tag_type0"] = cfg.view_show_tag_type[0];
    doc["tag_type1"] = cfg.view_show_tag_type[1];
    doc["show2loc0"] = cfg.view_show_secondary_locations[0];
    doc["show2loc1"] = cfg.view_show_secondary_locations[1];
    doc["bm_on"] = cfg.map_basemap_enabled;
    for (int i = 0; i < 6; i++) {
        char key[12];
        snprintf(key, sizeof(key), "bm_opa%d", i);
        doc[key] = cfg.map_basemap_opa[i];
    }
    doc["bm_opa"] = cfg.map_basemap_opa[0]; // legacy
    doc["bm_style"] = cfg.map_basemap_style;
    doc["wx_on"] = cfg.map_weather_enabled;
    doc["wx_opa"] = cfg.map_weather_opa;
    doc["last_view"] = cfg.last_view_idx;
    doc["last_rng"] = cfg.last_range_idx;
    doc["last_loc"] = cfg.last_location_name;

    FILE *f = fopen(config_file_path().c_str(), "w");
    if (!f) {
        platform_log("Storage: failed to open %s for writing\n", config_file_path().c_str());
        return;
    }
    std::string out;
    serializeJson(doc, out);
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    platform_log("Storage: config saved to %s\n", config_file_path().c_str());
}

void storage_factory_reset() {
    remove(config_file_path().c_str());
    platform_log("Storage: config file removed (factory reset)\n");
}
