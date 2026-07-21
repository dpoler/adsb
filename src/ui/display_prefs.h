#pragma once

// Runtime-only (not persisted, same as the filter bitmask in filters.h)
// toggle for whether Map/Radar draw callsign/altitude text next to each
// aircraft. Lives here rather than in filters.h since it isn't part of
// aircraft selection -- it doesn't hide any aircraft, just the label.
bool callsigns_hidden();
void callsigns_hidden_toggle();
