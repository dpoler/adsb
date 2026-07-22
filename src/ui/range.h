#pragma once

// Global shared range control — up to 4 user-configurable levels
#define RANGE_MAX_LEVELS 4

float range_get_nm();
int range_get_index();
void range_cycle();                              // advance to next level, wrapping
const char* range_label();                       // e.g. "50nm"
void range_set_default(int nm);                  // snap to nearest level
void range_set_index(int idx);                   // jump directly to a level index (clamped), e.g. resuming a persisted choice
void range_set_levels(const int *nm_values, int count); // load user presets (sorted descending internally)
