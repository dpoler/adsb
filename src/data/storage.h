#pragma once

struct UserConfig {
    char wifi_ssid[33];
    char wifi_pass[65];
    char airportdb_token[160]; // free token from airportdb.io — observed ~97 chars, sized with margin
    float home_lat;
    float home_lon;
    int home_elevation_ft;   // user-entered field elevation at Home, ft MSL --
                              // used for AGL calculations (e.g. the ascending/
                              // descending filter). Saved Locations get this
                              // from airportdb.io instead (Location::elevation_ft).
    int radius_nm;           // API query radius = max(radius_presets), set on save
    int radius_presets[4];  // user-configurable zoom levels, sorted ascending
    bool use_metric;
    bool use_ethernet;       // true=Ethernet, false=WiFi (default: WiFi)
    char watchlist[10][7]; // up to 10 ICAO hex codes
    int watchlist_count;

    // View cycle settings
    bool cycle_enabled;
    int cycle_interval_s;    // seconds between auto-advance (default 30)
    int cycle_inactivity_s;  // seconds before resuming cycling after touch (default 60)

    // Alert settings
    bool alert_military;     // show popup for military aircraft
    bool alert_emergency;    // show popup for squawk 7500/7600/7700

    int trail_style;         // 0=line, 1=dots -- unused (dead field, kept for NVS layout compat)

    // Display filters
    bool hide_ground;          // don't show aircraft with on_ground flag set

    // VIEW menu -- Map and Radar each get independent settings, indexed by
    // VIEW_MAP/VIEW_RADAR (views.h; 0/1 -- Arrivals/Stats have no VIEW chip
    // at all). Without this, switching views while the VIEW popover was
    // still open would leak one view's settings into the other (reported).
    // display_prefs.cpp's accessors resolve which slot to use via
    // views_get_active_index() -- callers never index these directly.
    // trails_enabled defaults true and trail_max_points 30 (both views,
    // matching pre-per-view behavior); show_tag_id defaults true (matches
    // the callsign label both views always showed before any of this
    // existed); show_tag_data/show_tag_type default false (new capability
    // -- Map never showed this before, stay minimal until turned on);
    // show_secondary_locations defaults true (matches the airport/HOME
    // markers always being drawn before this existed).
    bool view_trails_enabled[2];
    int view_trail_max_points[2];      // 10-60 (default 30)
    bool view_show_tag_id[2];          // flight number, falling back to registration then ICAO hex
    bool view_show_tag_data[2];        // altitude + speed + climb/descend arrow
    bool view_show_tag_type[2];        // aircraft type / operator
    bool view_show_secondary_locations[2]; // other saved/static airports + HOME-elsewhere marker

    // Resume-on-boot state -- all written from discrete, human-paced actions
    // (nav tap, range chip tap, location picker selection, filter button
    // tap), never from a high-frequency path like a slider drag, so an
    // immediate storage_save_config() on each change is safe (see the
    // trail-slider cyan-flash fix for why that distinction matters).
    int last_view_idx;              // VIEW_MAP/VIEW_RADAR/VIEW_ARRIVALS/VIEW_STATS (views.h)
    int last_range_idx;             // index into range.cpp's levels, 0 = widest
    char last_location_icao[8];     // matches LOC_ICAO_LEN (locations.h); "" = Home
    unsigned last_filter_mask;      // FILT_* bitmask (filters.h)
};

// Load config from NVS. Returns defaults if not found.
UserConfig storage_load_config();

// Save config to NVS
void storage_save_config(const UserConfig &cfg);

// Global runtime config — loaded at boot, updated on settings save
extern UserConfig g_config;
