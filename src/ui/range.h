#pragma once

// Global shared range control — three levels: 50, 20, 5 nm
#define RANGE_NUM_LEVELS 3

float range_get_nm();
int range_get_index();
void range_cycle();              // advance to next level, wrapping
const char* range_label();       // e.g. "50nm"
void range_set_default(int nm);  // snap to nearest level; call after loading config
