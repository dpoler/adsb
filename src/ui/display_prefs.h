#pragma once

// Per-field tag toggles and secondary-location visibility for Map/Radar,
// controlled by the status bar's VIEW menu (view_menu.cpp). Live here
// rather than in filters.h since none of this hides aircraft -- it's what
// gets drawn next to (or around) them. Persisted in g_config (storage.h)
// -- every toggle here is a single discrete tap, so an immediate NVS
// write on each call is safe (same reasoning as filter_toggle() in
// filters.cpp).

// Flight number, falling back to registration then ICAO hex if no
// callsign -- never shows registration alongside an existing callsign.
bool tag_id_shown();
void tag_id_toggle();

// Altitude + speed + climb/descend arrow.
bool tag_data_shown();
void tag_data_toggle();

// Aircraft type / operator.
bool tag_type_shown();
void tag_type_toggle();

// Other saved/static airports + the HOME-elsewhere marker.
bool secondary_locations_shown();
void secondary_locations_toggle();
