#include "storage.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>

UserConfig g_config = {};
static Preferences _prefs;

UserConfig storage_load_config() {
    UserConfig cfg;

    // Compiled defaults — credentials and location are blank until set via settings
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
    cfg.use_ethernet = false; // WiFi by default
    cfg.watchlist_count = 0;
    cfg.alert_military = true;
    cfg.alert_emergency = true;
    cfg.trail_style = 0;
    cfg.display_brightness_pct = 100;
    cfg.display_dim_after_min = 0;   // never dim
    cfg.display_blank_after_min = 0; // never blank
    cfg.screensaver_enabled = false;
    cfg.screensaver_drift = true;
    for (int i = 0; i < 4; i++) {
        cfg.view_filter_mask[i] = 0;  // no filters active
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
    cfg.map_basemap_style = 0; // Carto dark_all
    cfg.map_weather_enabled = false;
    cfg.map_weather_opa = 60;
    cfg.last_view_idx = 0;   // VIEW_MAP
    cfg.last_range_idx = 0;  // widest preset
    cfg.last_location_name[0] = '\0'; // nothing selected

    _prefs.begin("adsb", true); // read-only

    // Override with NVS values where they exist
    if (_prefs.isKey("ssid"))
        strlcpy(cfg.wifi_ssid, _prefs.getString("ssid", cfg.wifi_ssid).c_str(), sizeof(cfg.wifi_ssid));
    if (_prefs.isKey("pass"))
        strlcpy(cfg.wifi_pass, _prefs.getString("pass", cfg.wifi_pass).c_str(), sizeof(cfg.wifi_pass));
    if (_prefs.isKey("apt_tok"))
        strlcpy(cfg.airportdb_token, _prefs.getString("apt_tok", cfg.airportdb_token).c_str(), sizeof(cfg.airportdb_token));
    cfg.airportdb_enabled = _prefs.getBool("apt_en", cfg.airportdb_enabled);
    if (_prefs.isKey("adbox_key"))
        strlcpy(cfg.aerodatabox_key, _prefs.getString("adbox_key", cfg.aerodatabox_key).c_str(), sizeof(cfg.aerodatabox_key));
    cfg.aerodatabox_provider = _prefs.getInt("adbox_prov", cfg.aerodatabox_provider);
    if (cfg.aerodatabox_provider < 0 || cfg.aerodatabox_provider > 2) cfg.aerodatabox_provider = 0;
    cfg.aerodatabox_enabled = _prefs.getBool("adbox_en", cfg.aerodatabox_enabled);
    cfg.adbox_usage_yyyymm = _prefs.getInt("adbox_ym", cfg.adbox_usage_yyyymm);
    cfg.adbox_usage_count = _prefs.getInt("adbox_n", cfg.adbox_usage_count);
    cfg.adbox_soft_limit = _prefs.getInt("adbox_lim", cfg.adbox_soft_limit);
    cfg.adbox_rate_limited = _prefs.getBool("adbox_rl", cfg.adbox_rate_limited);
    cfg.radius_nm = _prefs.getInt("radius", cfg.radius_nm);
    cfg.radius_presets[0] = _prefs.getInt("rad0", cfg.radius_presets[0]);
    cfg.radius_presets[1] = _prefs.getInt("rad1", cfg.radius_presets[1]);
    cfg.radius_presets[2] = _prefs.getInt("rad2", cfg.radius_presets[2]);
    cfg.radius_presets[3] = _prefs.getInt("rad3", cfg.radius_presets[3]);
    cfg.use_metric = _prefs.getBool("metric", cfg.use_metric);
    cfg.use_ethernet = _prefs.getBool("use_eth", cfg.use_ethernet);
    cfg.alert_military = _prefs.getBool("alrt_mil", cfg.alert_military);
    cfg.alert_emergency = _prefs.getBool("alrt_emg", cfg.alert_emergency);
    cfg.trail_style = _prefs.getInt("trail_sty", cfg.trail_style);
    cfg.display_brightness_pct = _prefs.getInt("disp_bright", cfg.display_brightness_pct);
    cfg.display_dim_after_min = _prefs.getInt("disp_dimmin", cfg.display_dim_after_min);
    cfg.display_blank_after_min = _prefs.getInt("disp_blkmin", cfg.display_blank_after_min);
    cfg.screensaver_enabled = _prefs.getBool("ss_enabled", cfg.screensaver_enabled);
    cfg.screensaver_drift = _prefs.getBool("ss_drift", cfg.screensaver_drift);
    for (int i = 0; i < 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "filt_m%d", i);
        cfg.view_filter_mask[i] = _prefs.getUInt(key, cfg.view_filter_mask[i]);
        snprintf(key, sizeof(key), "hide_gnd%d", i);
        cfg.view_hide_ground[i] = _prefs.getBool(key, cfg.view_hide_ground[i]);
    }
    cfg.view_trails_enabled[0] = _prefs.getBool("trail_on0", cfg.view_trails_enabled[0]);
    cfg.view_trails_enabled[1] = _prefs.getBool("trail_on1", cfg.view_trails_enabled[1]);
    cfg.view_trail_max_points[0] = _prefs.getInt("trail_pts0", cfg.view_trail_max_points[0]);
    cfg.view_trail_max_points[1] = _prefs.getInt("trail_pts1", cfg.view_trail_max_points[1]);
    cfg.view_show_tag_id[0] = _prefs.getBool("tag_id0", cfg.view_show_tag_id[0]);
    cfg.view_show_tag_id[1] = _prefs.getBool("tag_id1", cfg.view_show_tag_id[1]);
    cfg.view_show_tag_data[0] = _prefs.getBool("tag_data0", cfg.view_show_tag_data[0]);
    cfg.view_show_tag_data[1] = _prefs.getBool("tag_data1", cfg.view_show_tag_data[1]);
    cfg.view_show_tag_type[0] = _prefs.getBool("tag_type0", cfg.view_show_tag_type[0]);
    cfg.view_show_tag_type[1] = _prefs.getBool("tag_type1", cfg.view_show_tag_type[1]);
    cfg.view_show_secondary_locations[0] = _prefs.getBool("show2loc0", cfg.view_show_secondary_locations[0]);
    cfg.view_show_secondary_locations[1] = _prefs.getBool("show2loc1", cfg.view_show_secondary_locations[1]);
    cfg.map_basemap_enabled = _prefs.getBool("bm_on", cfg.map_basemap_enabled);
    // Legacy single bm_opa seeds all styles if per-style keys are absent.
    int legacy_opa = _prefs.getInt("bm_opa", 50);
    if (legacy_opa < 10) legacy_opa = 10;
    if (legacy_opa > 100) legacy_opa = 100;
    for (int i = 0; i < 6; i++) {
        char key[12];
        snprintf(key, sizeof(key), "bm_opa%d", i);
        cfg.map_basemap_opa[i] = _prefs.getInt(key, legacy_opa);
        if (cfg.map_basemap_opa[i] < 10) cfg.map_basemap_opa[i] = 10;
        if (cfg.map_basemap_opa[i] > 100) cfg.map_basemap_opa[i] = 100;
    }
    cfg.map_basemap_style = _prefs.getInt("bm_style", cfg.map_basemap_style);
    if (cfg.map_basemap_style < 0) cfg.map_basemap_style = 0;
    if (cfg.map_basemap_style > 5) cfg.map_basemap_style = 5;
    cfg.map_weather_enabled = _prefs.getBool("wx_on", cfg.map_weather_enabled);
    cfg.map_weather_opa = _prefs.getInt("wx_opa", cfg.map_weather_opa);
    if (cfg.map_weather_opa < 10) cfg.map_weather_opa = 10;
    if (cfg.map_weather_opa > 100) cfg.map_weather_opa = 100;
    cfg.last_view_idx = _prefs.getInt("last_view", cfg.last_view_idx);
    cfg.last_range_idx = _prefs.getInt("last_rng", cfg.last_range_idx);
    if (_prefs.isKey("last_loc"))
        strlcpy(cfg.last_location_name, _prefs.getString("last_loc", cfg.last_location_name).c_str(), sizeof(cfg.last_location_name));

    _prefs.end();
    Serial.println("Storage: config loaded from NVS");
    return cfg;
}

void storage_save_config(const UserConfig &cfg) {
    _prefs.begin("adsb", false); // read-write

    _prefs.putString("ssid", cfg.wifi_ssid);
    _prefs.putString("pass", cfg.wifi_pass);
    _prefs.putString("apt_tok", cfg.airportdb_token);
    _prefs.putBool("apt_en", cfg.airportdb_enabled);
    _prefs.putString("adbox_key", cfg.aerodatabox_key);
    _prefs.putInt("adbox_prov", cfg.aerodatabox_provider);
    _prefs.putBool("adbox_en", cfg.aerodatabox_enabled);
    _prefs.putInt("adbox_ym", cfg.adbox_usage_yyyymm);
    _prefs.putInt("adbox_n", cfg.adbox_usage_count);
    _prefs.putInt("adbox_lim", cfg.adbox_soft_limit);
    _prefs.putBool("adbox_rl", cfg.adbox_rate_limited);
    _prefs.putInt("radius", cfg.radius_nm);
    _prefs.putInt("rad0", cfg.radius_presets[0]);
    _prefs.putInt("rad1", cfg.radius_presets[1]);
    _prefs.putInt("rad2", cfg.radius_presets[2]);
    _prefs.putInt("rad3", cfg.radius_presets[3]);
    _prefs.putBool("metric", cfg.use_metric);
    _prefs.putBool("use_eth", cfg.use_ethernet);
    _prefs.putBool("alrt_mil", cfg.alert_military);
    _prefs.putBool("alrt_emg", cfg.alert_emergency);
    _prefs.putInt("trail_sty", cfg.trail_style);
    _prefs.putInt("disp_bright", cfg.display_brightness_pct);
    _prefs.putInt("disp_dimmin", cfg.display_dim_after_min);
    _prefs.putInt("disp_blkmin", cfg.display_blank_after_min);
    _prefs.putBool("ss_enabled", cfg.screensaver_enabled);
    _prefs.putBool("ss_drift", cfg.screensaver_drift);
    for (int i = 0; i < 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "filt_m%d", i);
        _prefs.putUInt(key, cfg.view_filter_mask[i]);
        snprintf(key, sizeof(key), "hide_gnd%d", i);
        _prefs.putBool(key, cfg.view_hide_ground[i]);
    }
    _prefs.putBool("trail_on0", cfg.view_trails_enabled[0]);
    _prefs.putBool("trail_on1", cfg.view_trails_enabled[1]);
    _prefs.putInt("trail_pts0", cfg.view_trail_max_points[0]);
    _prefs.putInt("trail_pts1", cfg.view_trail_max_points[1]);
    _prefs.putBool("tag_id0", cfg.view_show_tag_id[0]);
    _prefs.putBool("tag_id1", cfg.view_show_tag_id[1]);
    _prefs.putBool("tag_data0", cfg.view_show_tag_data[0]);
    _prefs.putBool("tag_data1", cfg.view_show_tag_data[1]);
    _prefs.putBool("tag_type0", cfg.view_show_tag_type[0]);
    _prefs.putBool("tag_type1", cfg.view_show_tag_type[1]);
    _prefs.putBool("show2loc0", cfg.view_show_secondary_locations[0]);
    _prefs.putBool("show2loc1", cfg.view_show_secondary_locations[1]);
    _prefs.putBool("bm_on", cfg.map_basemap_enabled);
    for (int i = 0; i < 6; i++) {
        char key[12];
        snprintf(key, sizeof(key), "bm_opa%d", i);
        _prefs.putInt(key, cfg.map_basemap_opa[i]);
    }
    // Keep legacy bm_opa as style 0 so older builds still read something sensible.
    _prefs.putInt("bm_opa", cfg.map_basemap_opa[0]);
    _prefs.putInt("bm_style", cfg.map_basemap_style);
    _prefs.putBool("wx_on", cfg.map_weather_enabled);
    _prefs.putInt("wx_opa", cfg.map_weather_opa);
    _prefs.putInt("last_view", cfg.last_view_idx);
    _prefs.putInt("last_rng", cfg.last_range_idx);
    _prefs.putString("last_loc", cfg.last_location_name);

    _prefs.end();
    Serial.println("Storage: config saved to NVS");
}

void storage_factory_reset() {
    _prefs.begin("adsb", false);
    _prefs.clear();
    _prefs.end();
    Serial.println("Storage: config namespace erased (factory reset)");
}
