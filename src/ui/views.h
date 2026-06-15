#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// View indices
#define VIEW_MAP 0
#define VIEW_RADAR 1
#define VIEW_ARRIVALS 2
#define VIEW_STATS 3

// Initialize the tileview with all view containers
void views_init(lv_obj_t *parent, AircraftList *list);

// Get the container object for a specific view (for adding child widgets)
lv_obj_t *views_get_tile(int view_index);

// Get the currently active tile index
int views_get_active_index();

// Get the tileview object (for view cycling)
lv_obj_t *views_get_tileview();

// Switch to a view by index — updates all state immediately, no tileview callback needed
void views_switch_to(int idx);

// Pause auto-cycle (call when user manually selects a view)
void views_pause_cycle();

// Attach manual left/right swipe detection to obj (PRESSING-based, no tileview animation)
void views_attach_swipe(lv_obj_t *obj);

// Returns true if a swipe just completed (use to suppress tap handlers in CLICKED callbacks)
bool views_swipe_active();
void views_clear_swipe();

// Global touch state — set by touch_read_cb, used by view timers to defer rendering
extern volatile bool touch_active;
