#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

void radar_view_init(lv_obj_t *parent, AircraftList *list);
void radar_view_update();
void radar_view_set_home(float lat, float lon);

// Clear trails on this view only -- see status_bar.cpp's shared CLR chip
void radar_view_clear_trails();
