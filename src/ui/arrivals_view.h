#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

void arrivals_view_init(lv_obj_t *parent, AircraftList *list);
void arrivals_view_update();
void arrivals_view_on_show();
