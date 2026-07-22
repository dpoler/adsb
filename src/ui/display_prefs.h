#pragma once

// Whether Map/Radar draw callsign/altitude text next to each aircraft (the
// status bar's TAG chip). Lives here rather than in filters.h since it
// isn't part of aircraft selection -- it doesn't hide any aircraft, just
// the label. Persisted in g_config.hide_callsigns (storage.h) -- toggling
// is always a single discrete tap, so an immediate NVS write on every
// call is safe (same reasoning as filter_toggle() in filters.cpp).
bool callsigns_hidden();
void callsigns_hidden_toggle();
