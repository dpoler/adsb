#pragma once
#include "lvgl.h"

// Creates the location-picker button (top-left, under the status bar) and
// wires up its popover: Home + saved airports, plus "Add by ICAO".
void location_picker_init(lv_obj_t *screen);
