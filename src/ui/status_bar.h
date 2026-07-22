#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// Single source of truth for the status bar's height -- every view that
// needs to know where its own canvas starts (map/radar/arrivals/stats) or
// how tall the popover overlays below it can be (location_picker) includes
// this instead of hardcoding the number.
#define STATUS_BAR_HEIGHT 48

// x position of the location picker chip (location_picker.cpp) -- shared
// here so status_bar.cpp's range chip (which sits directly beside it) can
// stay in sync without hardcoding the picker's position a second time.
#define LOCATION_CHIP_X 140

// Create the status bar at the top of the screen (STATUS_BAR_HEIGHT tall)
lv_obj_t *status_bar_create(lv_obj_t *parent);

// Update status bar with current data
void status_bar_update(bool wifi_connected, int aircraft_count, uint32_t last_update_ms);

// Update the active view dot indicator
void status_bar_set_active_dot(int view_index);

// Set callback for gear icon tap
void status_bar_set_gear_callback(lv_event_cb_t cb);

// Show/hide the AUTO cycle indicator near view dots
void status_bar_set_auto_indicator(bool visible);

// Screen x of the VIEW chip -- its own popover (view_menu.cpp) anchors
// under this instead of a hardcoded position, since (unlike the location
// picker chip at the fixed LOCATION_CHIP_X) this chip's x depends on
// NUM_VIEWS/screen width and is only known once the bar is actually built.
int status_bar_get_view_chip_x();
