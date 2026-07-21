#pragma once
#include <lvgl.h>

// Quick-settings popover for the status bar's TRAIL chip -- on/off, length,
// and clear-now all in one place instead of split between Settings (on/off,
// length) and the chip's old momentary clear action.
void trail_menu_toggle();

// Closes the popover if open -- called by location_picker.cpp so only one
// status-bar popover can be open at a time.
void trail_menu_close();
