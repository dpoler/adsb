#include "storage.h"
#include <Preferences.h>
#include <cstring>

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
    cfg.trails_enabled = true;
    cfg.trail_max_points = 30;
    cfg.trail_style = 0;
    cfg.hide_ground = false;
    cfg.last_view_idx = 0;   // VIEW_MAP
    cfg.last_range_idx = 0;  // widest preset
    cfg.last_location_icao[0] = '\0'; // Home
    cfg.last_filter_mask = 0; // no filters active

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
    cfg.trails_enabled = _prefs.getBool("trail_on", cfg.trails_enabled);
    cfg.trail_max_points = _prefs.getInt("trail_pts", cfg.trail_max_points);
    cfg.trail_style = _prefs.getInt("trail_sty", cfg.trail_style);
    cfg.hide_ground = _prefs.getBool("hide_gnd", cfg.hide_ground);
    cfg.last_view_idx = _prefs.getInt("last_view", cfg.last_view_idx);
    cfg.last_range_idx = _prefs.getInt("last_rng", cfg.last_range_idx);
    if (_prefs.isKey("last_loc"))
        strlcpy(cfg.last_location_icao, _prefs.getString("last_loc", cfg.last_location_icao).c_str(), sizeof(cfg.last_location_icao));
    cfg.last_filter_mask = _prefs.getUInt("last_filt", cfg.last_filter_mask);

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
    _prefs.putBool("trail_on", cfg.trails_enabled);
    _prefs.putInt("trail_pts", cfg.trail_max_points);
    _prefs.putInt("trail_sty", cfg.trail_style);
    _prefs.putBool("hide_gnd", cfg.hide_ground);
    _prefs.putInt("last_view", cfg.last_view_idx);
    _prefs.putInt("last_rng", cfg.last_range_idx);
    _prefs.putString("last_loc", cfg.last_location_icao);
    _prefs.putUInt("last_filt", cfg.last_filter_mask);

    _prefs.end();
    Serial.println("Storage: config saved to NVS");
}
