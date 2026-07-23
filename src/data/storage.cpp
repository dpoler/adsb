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
    cfg.home_lat = 0.0f;
    cfg.home_lon = 0.0f;
    cfg.home_elevation_ft = 0;
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
    cfg.cycle_enabled = true;
    cfg.cycle_interval_s = 15;
    cfg.cycle_inactivity_s = 60;
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
    cfg.last_view_idx = 0;   // VIEW_MAP
    cfg.last_range_idx = 0;  // widest preset
    cfg.last_location_icao[0] = '\0'; // Home

    _prefs.begin("adsb", true); // read-only

    // Override with NVS values where they exist
    if (_prefs.isKey("ssid"))
        strlcpy(cfg.wifi_ssid, _prefs.getString("ssid", cfg.wifi_ssid).c_str(), sizeof(cfg.wifi_ssid));
    if (_prefs.isKey("pass"))
        strlcpy(cfg.wifi_pass, _prefs.getString("pass", cfg.wifi_pass).c_str(), sizeof(cfg.wifi_pass));
    if (_prefs.isKey("apt_tok"))
        strlcpy(cfg.airportdb_token, _prefs.getString("apt_tok", cfg.airportdb_token).c_str(), sizeof(cfg.airportdb_token));
    cfg.home_lat = _prefs.getFloat("lat", cfg.home_lat);
    cfg.home_lon = _prefs.getFloat("lon", cfg.home_lon);
    cfg.home_elevation_ft = _prefs.getInt("home_elev", cfg.home_elevation_ft);
    cfg.radius_nm = _prefs.getInt("radius", cfg.radius_nm);
    cfg.radius_presets[0] = _prefs.getInt("rad0", cfg.radius_presets[0]);
    cfg.radius_presets[1] = _prefs.getInt("rad1", cfg.radius_presets[1]);
    cfg.radius_presets[2] = _prefs.getInt("rad2", cfg.radius_presets[2]);
    cfg.radius_presets[3] = _prefs.getInt("rad3", cfg.radius_presets[3]);
    cfg.use_metric = _prefs.getBool("metric", cfg.use_metric);
    cfg.use_ethernet = _prefs.getBool("use_eth", cfg.use_ethernet);
    cfg.alert_military = _prefs.getBool("alrt_mil", cfg.alert_military);
    cfg.alert_emergency = _prefs.getBool("alrt_emg", cfg.alert_emergency);
    cfg.cycle_enabled = _prefs.getBool("cyc_on", cfg.cycle_enabled);
    cfg.cycle_interval_s = _prefs.getInt("cyc_int", cfg.cycle_interval_s);
    cfg.cycle_inactivity_s = _prefs.getInt("cyc_idle", cfg.cycle_inactivity_s);
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
    cfg.last_view_idx = _prefs.getInt("last_view", cfg.last_view_idx);
    cfg.last_range_idx = _prefs.getInt("last_rng", cfg.last_range_idx);
    if (_prefs.isKey("last_loc"))
        strlcpy(cfg.last_location_icao, _prefs.getString("last_loc", cfg.last_location_icao).c_str(), sizeof(cfg.last_location_icao));

    _prefs.end();
    Serial.println("Storage: config loaded from NVS");
    return cfg;
}

void storage_save_config(const UserConfig &cfg) {
    _prefs.begin("adsb", false); // read-write

    _prefs.putString("ssid", cfg.wifi_ssid);
    _prefs.putString("pass", cfg.wifi_pass);
    _prefs.putString("apt_tok", cfg.airportdb_token);
    _prefs.putFloat("lat", cfg.home_lat);
    _prefs.putFloat("lon", cfg.home_lon);
    _prefs.putInt("home_elev", cfg.home_elevation_ft);
    _prefs.putInt("radius", cfg.radius_nm);
    _prefs.putInt("rad0", cfg.radius_presets[0]);
    _prefs.putInt("rad1", cfg.radius_presets[1]);
    _prefs.putInt("rad2", cfg.radius_presets[2]);
    _prefs.putInt("rad3", cfg.radius_presets[3]);
    _prefs.putBool("metric", cfg.use_metric);
    _prefs.putBool("use_eth", cfg.use_ethernet);
    _prefs.putBool("alrt_mil", cfg.alert_military);
    _prefs.putBool("alrt_emg", cfg.alert_emergency);
    _prefs.putBool("cyc_on", cfg.cycle_enabled);
    _prefs.putInt("cyc_int", cfg.cycle_interval_s);
    _prefs.putInt("cyc_idle", cfg.cycle_inactivity_s);
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
    _prefs.putInt("last_view", cfg.last_view_idx);
    _prefs.putInt("last_rng", cfg.last_range_idx);
    _prefs.putString("last_loc", cfg.last_location_icao);

    _prefs.end();
    Serial.println("Storage: config saved to NVS");
}
