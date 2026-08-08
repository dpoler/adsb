#pragma once

struct UserConfig {
    char wifi_ssid[33];
    char wifi_pass[65];
    char airportdb_token[160]; // free token from airportdb.io — observed ~97 chars, sized with margin
    bool airportdb_enabled;  // use airportdb.io when adding airports (key still required)
    // AeroDataBox flight status (origin/destination). Key is hand-edited into
    // config on Pi; serial TOKEN-style entry not wired for this key yet.
    // Provider selects which marketplace gateway + auth header to use:
    // 0=RapidAPI, 1=API.Market, 2=Direct (api.aerodatabox.com).
    char aerodatabox_key[80];
    int aerodatabox_provider;
    bool aerodatabox_enabled; // detail-card O/D enrichment when key is present+valid
    // Local AeroDataBox usage accounting (marketplace remaining-units are NOT
    // exposed by RapidAPI/API.Market via the API key — only their dashboards
    // know the real quota). We track calls we make and can soft-cap / react
    // to HTTP 429.
    int adbox_usage_yyyymm;   // calendar month of adbox_usage_count, e.g. 202608
    int adbox_usage_count;    // AeroDataBox HTTP calls this month
    int adbox_soft_limit;     // 0 = no local cap; else auto-disable at count
    bool adbox_rate_limited;  // sticky: hit HTTP 429 (or soft limit)
    int radius_nm;           // API query radius = max(radius_presets), set on save
    int radius_presets[4];  // user-configurable zoom levels, sorted ascending
    bool use_metric;
    bool use_ethernet;       // true=Ethernet, false=WiFi (default: WiFi)
    char watchlist[10][7]; // up to 10 ICAO hex codes
    int watchlist_count;

    // Alert settings
    bool alert_military;     // show popup for military aircraft
    bool alert_emergency;    // show popup for squawk 7500/7600/7700

    int trail_style;         // 0=line, 1=dots -- unused (dead field, kept for NVS layout compat)

    // Display sleep / brightness / screensaver (screensaver.cpp) -- all
    // idle timers count from LVGL's own input-activity clock
    // (lv_display_get_inactive_time()), not a custom touch tracker.
    int display_brightness_pct;   // 10-100, backlight level when active/normal (default 100)
    int display_dim_after_min;    // idle minutes before dimming; 0 = never dim (default 0)
    int display_blank_after_min;  // idle minutes before blank/screensaver; 0 = never (default 0)
    bool screensaver_enabled;     // show the moving aircraft-count screensaver instead of a
                                   // plain backlight-off blank once display_blank_after_min hits
    bool screensaver_drift;       // true = drift continuously around the screen, false = jump
                                   // to a new position every 20s

    // Display filters -- FILT_* bitmask and GND, indexed by VIEW_MAP/
    // VIEW_RADAR/VIEW_ARRIVALS (views.h; VIEW_STATS unused -- no filter
    // column there). Map/Radar/List (Arrivals) each remember their own
    // filter selection and GND state independently. GND is a separate
    // unconditional exclude, not part of the FILT_* bitmask (see
    // filters.h) -- kept per-view alongside it since the two are
    // mutually-exclusive partners (map_view.cpp etc.) and leaving GND
    // global while VERT went per-view would reopen the exact
    // cross-view-leak bug already fixed for the VIEW menu below.
    // filters.cpp/each view file resolve which slot to use themselves
    // (filters.cpp via views_filterable_index(); each view file already
    // knows its own VIEW_* constant, no resolution needed).
    unsigned view_filter_mask[4];   // FILT_* bitmask (filters.h)
    bool view_hide_ground[4];       // GND quick-toggle

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
    bool view_show_secondary_locations[2]; // other saved/static airports

    // Pi Map basemap (Carto / FAA sectional tiles under Map). Unused on ESP32
    // / Radar but kept in the shared UserConfig so Pi JSON and ESP32 NVS key
    // sets stay aligned. Defaults: on at 50% opacity, Carto dark_all.
    // Style: 0=Carto dark, 1=Carto dark (no labels), 2=FAA VFR sectional,
    // 3=Carto voyager (cream light), 4=voyager no labels, 5=OpenTopoMap.
    // Opacity is per-style.
    bool map_basemap_enabled;
    int map_basemap_opa[6];         // 10-100 percent per style (default 50)
    int map_basemap_style;          // see display_prefs / basemap styles

    // Pi Map weather overlay (RainViewer precip radar). Unused on ESP32 /
    // Radar but kept in shared UserConfig. Defaults: off at 60% opacity.
    bool map_weather_enabled;
    int map_weather_opa;            // 10-100 percent (default 60)

    // Resume-on-boot state -- all written from discrete, human-paced actions
    // (nav tap, range chip tap, location picker selection, filter button
    // tap), never from a high-frequency path like a slider drag, so an
    // immediate storage_save_config() on each change is safe (see the
    // trail-slider cyan-flash fix for why that distinction matters).
    int last_view_idx;              // VIEW_MAP/VIEW_RADAR/VIEW_ARRIVALS/VIEW_STATS (views.h)
    int last_range_idx;             // index into range.cpp's levels, 0 = widest
    char last_location_name[17];    // matches LOC_NAME_LEN (locations.h); "" = none selected.
                                     // Matched against Location::name, not icao -- works
                                     // uniformly for airports (name==icao) and waypoints
                                     // (name is whatever the user typed, no icao at all).
};

// Load config from NVS. Returns defaults if not found.
UserConfig storage_load_config();

// Save config to NVS
void storage_save_config(const UserConfig &cfg);

// Erases the entire "adsb" NVS namespace (every UserConfig field -- WiFi,
// Home, radius presets, filters, everything). Does not touch g_config in
// memory or reload it -- caller is expected to reboot immediately
// (serial_config.cpp's FACTORY_RESET does), at which point storage_load_config()
// picks up compiled defaults from the now-empty namespace.
void storage_factory_reset();

// Global runtime config — loaded at boot, updated on settings save
extern UserConfig g_config;
