#include "range.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

static float _levels[RANGE_MAX_LEVELS] = {50.0f, 20.0f, 10.0f, 5.0f};
static int _count = 4;
static int _idx = 0;
static char _label_buf[8];

float range_get_nm() {
    return _levels[_idx];
}

int range_get_index() {
    return _idx;
}

void range_cycle() {
    _idx = (_idx + 1) % _count;
}

const char* range_label() {
    snprintf(_label_buf, sizeof(_label_buf), "%dnm", (int)_levels[_idx]);
    return _label_buf;
}

void range_set_levels(const int *nm_values, int count) {
    if (count < 1) count = 1;
    if (count > RANGE_MAX_LEVELS) count = RANGE_MAX_LEVELS;
    _count = count;
    for (int i = 0; i < count; i++)
        _levels[i] = (float)nm_values[i];
    // Sort descending so index 0 = largest range (widest view first)
    for (int i = 0; i < _count - 1; i++)
        for (int j = i + 1; j < _count; j++)
            if (_levels[j] > _levels[i]) {
                float tmp = _levels[i]; _levels[i] = _levels[j]; _levels[j] = tmp;
            }
    if (_idx >= _count) _idx = 0;
}

void range_set_index(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= _count) idx = _count - 1;
    _idx = idx;
}

void range_set_default(int nm) {
    int best = 0;
    float best_diff = fabsf((float)nm - _levels[0]);
    for (int i = 1; i < _count; i++) {
        float diff = fabsf((float)nm - _levels[i]);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    _idx = best;
}
