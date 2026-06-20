#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"
#include "geo.h"

void map_view_init(lv_obj_t *parent, AircraftList *list);
void map_view_update();
void map_view_on_show();

// Center the map on a specific lat/lon and redraw
void map_view_center_on(float lat, float lon);

// Set which aircraft has the tracking circle (by ICAO hex, nullptr to clear)
void map_view_track(const char *icao_hex);

// Number of aircraft drawn in the last frame (matches what's visible on screen)
int map_view_drawn_count();
