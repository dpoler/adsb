#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// View indices — runway diagrams live inside Map view (see map_view.cpp),
// driven by the location picker rather than being a swipeable tile of its own.
#define VIEW_MAP      0
#define VIEW_RADAR    1
#define VIEW_ARRIVALS 2
#define VIEW_STATS    3
#define NUM_VIEWS     4

// Initialize the tileview with all view containers
void views_init(lv_obj_t *parent, AircraftList *list);

// Get the container object for a specific view (for adding child widgets)
lv_obj_t *views_get_tile(int view_index);

// Get the currently active tile index
int views_get_active_index();

// Resolves the active view to one of VIEW_MAP/VIEW_RADAR/VIEW_ARRIVALS --
// falls back to VIEW_MAP if Stats is active (no filter column, no GND
// button there) or the index is otherwise out of range. Shared by any
// per-view setting that Map/Radar/Arrivals(List) each keep independently
// (filters.cpp's FILT_* bitmask + GND; see storage.h).
int views_filterable_index();

// Get the tileview object (for view cycling)
lv_obj_t *views_get_tileview();

// Screen-absolute Y of the top edge of the tileview's own horizontal
// scrollbar ("swipe bar") -- queried directly from LVGL (lv_obj_get_
// scrollbar_area()) rather than guessed from theme constants, since its
// exact position/thickness depends on the active theme's DPI-scaled
// padding. Map/Radar center their range rings/compass between this and
// the status bar rather than assuming the tile's full declared height is
// usable -- content drawn past this line would sit under/behind where the
// scrollbar renders. Falls back to a conservative estimate if LVGL
// reports no scrollbar area (e.g. scrollbar mode changed, or queried
// before the tileview has ever laid out/scrolled).
int views_get_swipe_bar_top();

// Switch to a view by index — updates all state immediately, no tileview callback needed
void views_switch_to(int idx);

// Resume the view active at last shutdown/reboot (g_config.last_view_idx).
// Call after detail_card_init()/alerts_init() -- see views.cpp for why.
void views_resume_last_view();

// Pause auto-cycle (call when user manually selects a view)
void views_pause_cycle();

// Attach manual left/right swipe detection to obj (PRESSING-based, no tileview animation)
void views_attach_swipe(lv_obj_t *obj);

// Returns true if a swipe just completed (use to suppress tap handlers in CLICKED callbacks)
bool views_swipe_active();
void views_clear_swipe();

// Global touch state — set by touch_read_cb, used by view timers to defer rendering
extern volatile bool touch_active;
