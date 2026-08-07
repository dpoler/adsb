#include "lvgl.h"
#include "display.h"
#include "../src/platform/platform.h"
#include "../src/data/aircraft.h"
#include "../src/data/storage.h"
#include "../src/data/datasource.h"
#include "../src/data/fetcher.h"
#include "../src/data/locations.h"
#include "../src/data/airlines.h"
#include "../src/data/enrichment.h"
#include "../src/data/metar.h"
#include "../src/data/atis.h"
#include "../src/ui/views.h"
#include "../src/ui/detail_card.h"
#include "../src/ui/range.h"
#include "../src/ui/settings.h"
#include "../src/ui/status_bar.h"
#include "../src/ui/filters.h"
#include "../src/ui/map_view.h"
#include "../src/ui/stats.h"
#include "../src/ui/geo.h"
#include "../src/ui/alerts.h"
#include "../src/ui/location_picker.h"
#include "basemap.h"
#include <chrono>
#include <cstring>
#include <thread>

// Milestone 6 of the Pi port (see project_pi_port memory / pi-port
// branch's plan): the real status bar (nav tabs, gear icon, range chip),
// replacing milestone 5's ad-hoc floating gear button. Nav tabs give real
// tap-to-navigate, a welcome alternative to swipe after the whole
// scroll/swipe debugging saga. No location chip yet (location_picker.cpp
// isn't ported -- see pi/app_stubs.cpp's comment), so there's a small gap
// at LOCATION_CHIP_X where it would normally sit. VIEW chip is present and
// correctly shows/hides on Map/Radar but doesn't open anything yet
// (view_menu.cpp not ported, see pi/app_stubs.cpp).

AircraftList aircraft_list;

// Read by map_view.cpp/radar_view.cpp to defer heavy rendering during a
// touch drag (views.h). Always false here for now -- SDL/libinput aren't
// wired to update it the way the ESP32 touch driver does in its own
// main.cpp, and it's a perf optimization rather than a correctness
// requirement, one the Pi's far larger compute budget needs far less than
// the ESP32-P4 does anyway.
volatile bool touch_active = false;

extern void pi_fetcher_stats_update(bool ok, uint32_t elapsed_ms);
extern void pi_wait_for_next_fetch(int seconds);

static void fetch_loop() {
    RemoteApiDataSource src;
    while (true) {
        uint32_t t0 = platform_millis();
        bool ok = src.fetch(&aircraft_list);
        uint32_t elapsed = platform_millis() - t0;
        pi_fetcher_stats_update(ok, elapsed);
        platform_log("Fetch (%s): %s, %d aircraft tracked (%ums)\n",
                      src.name(), ok ? "OK" : "FAILED", aircraft_list.count, elapsed);
        // Same cadence as ESP32 location_poll_task: METAR/ATIS are
        // internally rate-limited (loc change or every few minutes).
        metar_poll();
        atis_poll();
        // Waits up to 20s, but returns immediately if
        // fetcher_request_immediate_fetch() is called in the meantime
        // (locations_set_active() calls it on every location switch).
        pi_wait_for_next_fetch(20);
    }
}

static uint32_t pi_tick_cb() {
    return platform_millis();
}

int main() {
    aircraft_list.init();

    // Real round-trip through pi/platform_linux/storage_linux.cpp --
    // proves the JSON-file config storage actually works, not just links.
    g_config = storage_load_config();
    storage_save_config(g_config);
    locations_init();

    // Airline code→name table (dpoler/AirlinesCSV). ESP32 loads this once
    // inside fetcher_init()'s boot task; Pi has no equivalent, so kick it
    // here on a background thread — blocking curl on the UI thread would
    // stall first paint, and detail_card airline_lookup() simply returns
    // nullptr until this finishes (falls back to owner_op).
    std::thread([] { airlines_load(); }).detach();

    std::thread fetch_thread(fetch_loop);
    fetch_thread.detach();

    lv_init();
    lv_tick_set_cb(pi_tick_cb);

    lv_display_t *disp = pi_display_init();
    pi_input_init(disp);

    enrichment_init();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    range_set_levels(g_config.radius_presets, 4);
    range_set_index(g_config.last_range_idx);

    status_bar_create(screen);
    views_init(screen, &aircraft_list);
    location_picker_init(screen);
    detail_card_init(screen, &aircraft_list);
    alerts_init(screen);
    views_resume_last_view();

    // Force LVGL to resolve the tileview's 4 tiles' lv_pct()-based
    // positions into real pixel coordinates right now. Without this, the
    // 4th tile (Stats) was found sitting at (0,0) -- on top of Map --
    // until *something* eventually triggered a layout pass; the
    // tileview's own scrollable-content width got computed (and
    // effectively cached) against that wrong position first, capping
    // real scrolling at 3 tiles' worth (confirmed via lv_obj_get_scroll_right()
    // reading 2560 instead of 3840 before this call, 3840 after). The
    // ESP32 main.cpp never hits this: it creates a lot more (status bar,
    // location picker, alerts, settings, OTA overlay) between views_init()
    // and its first real frame, incidentally forcing enough layout work
    // to resolve this as a side effect. This main.cpp doesn't do enough
    // of that on its own, so it needs an explicit nudge.
    lv_obj_update_layout(views_get_tileview());

    settings_init(screen);
    status_bar_set_gear_callback([](lv_event_t *) { settings_show(); });

    settings_set_change_callback([](const UserConfig *cfg) {
        bool presets_changed = (memcmp(cfg->radius_presets, g_config.radius_presets,
                                       sizeof(g_config.radius_presets)) != 0);
        g_config = *cfg;
        range_set_levels(cfg->radius_presets, 4);
        if (presets_changed) {
            range_set_default(cfg->radius_nm);
            g_config.last_range_idx = range_get_index();
            storage_save_config(g_config);
        }
    });

    // Periodic status bar update -- same aircraft-count logic as ESP32's
    // main.cpp (opacity/filter/hide_ground/radius, matching what a view
    // would actually draw), so the count agrees with what's on screen
    // rather than being sourced from any one view's own drawn-count cache.
    lv_timer_create([](lv_timer_t *t) {
        int count = 0;
        float center_lat, center_lon;
        locations_get_active_coords(&center_lat, &center_lon, nullptr);
        float radius_nm = range_get_nm();
        bool is_map = (views_filterable_index() == VIEW_MAP);
        if (aircraft_list.lock(5)) {
            uint32_t now = platform_millis();
            for (int i = 0; i < aircraft_list.count; i++) {
                Aircraft &ac = aircraft_list.aircraft[i];
                if (compute_aircraft_opacity(ac.stale_since, now) == 0) continue;
                if (!aircraft_passes_filter(ac)) continue;
                if (g_config.view_hide_ground[views_filterable_index()] && ac.on_ground) continue;
                if (is_map) {
                    if (!map_view_aircraft_visible(ac.lat, ac.lon)) continue;
                } else {
                    if (MapProjection::distance_nm(center_lat, center_lon, ac.lat, ac.lon) > radius_nm) continue;
                }
                count++;
            }
            aircraft_list.unlock();
        }
        status_bar_update(fetcher_wifi_connected(), count, stats_get()->current_count, fetcher_last_update());
        // While Map is downloading tiles, refresh the upper-right "Map N%" often.
        int bm_pct = 0;
        lv_timer_set_period(t, basemap_updating(&bm_pct) ? 200 : 1000);
        (void)bm_pct;
    }, 1000, nullptr);

    // A fixed ~1ms cadence, NOT lv_timer_handler()'s own returned "next
    // timer due" hint -- that value can be large (hundreds of ms) when
    // nothing's animating, and sleeping for it starves SDL's event pump
    // (lv_sdl_window.c's internal sdl_event_handler() timer, which drains
    // the OS mouse-motion queue via SDL_PollEvent() -- only serviced when
    // lv_timer_handler() actually runs). ESP32's main.cpp loop() never
    // needs to think about this: it already ignores lv_timer_handler()'s
    // return value and just runs a fixed 1ms vTaskDelay every iteration,
    // unconditionally -- matched here.
    while (true) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
