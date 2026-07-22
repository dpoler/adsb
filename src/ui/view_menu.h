#pragma once
#include <lvgl.h>

// Quick-settings popover for the status bar's VIEW chip -- trails (on/off,
// amount, clear-now), per-field tag toggles (flight ID, alt/speed, type),
// and secondary-location visibility, all in one place. Replaces the old
// separate TRAIL and TAG chips.
void view_menu_toggle();

// Closes the popover if open -- called by location_picker.cpp so only one
// status-bar popover can be open at a time.
void view_menu_close();
