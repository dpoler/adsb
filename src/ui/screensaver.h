#pragma once
#include "lvgl.h"

// Sets the LCD backlight level, 0-100 (jd9165_lcd::example_bsp_set_lcd_backlight
// via main.cpp -- kept as a callback instead of exposing the lcd instance
// itself outside main.cpp).
typedef void (*backlight_set_fn)(int percent);

// Builds the (hidden) full-screen screensaver overlay and starts the
// idle-driven dim/blank/screensaver state machine. Call once from main.cpp's
// setup(), after g_config is loaded and status_bar_create() has run (the
// screensaver's aircraft-count text reads status_bar_get_aircraft_count()).
void screensaver_init(lv_obj_t *screen, backlight_set_fn set_backlight);

// Opens the Display/Screensaver settings popover -- called from a button in
// settings.cpp.
void screensaver_show_settings();
