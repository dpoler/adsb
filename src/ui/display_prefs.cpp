#include "display_prefs.h"
#include "views.h"
#include "../data/storage.h"

// Arrivals/Stats have no VIEW chip and never call these -- default to
// Map's slot rather than depend on undefined behavior if one somehow
// were called outside a Map/Radar context.
static int active_view_idx() {
    return (views_get_active_index() == VIEW_RADAR) ? 1 : 0;
}

bool trails_shown() { return g_config.view_trails_enabled[active_view_idx()]; }
void trails_toggle() {
    int i = active_view_idx();
    g_config.view_trails_enabled[i] = !g_config.view_trails_enabled[i];
    storage_save_config(g_config);
}

int trails_amount() { return g_config.view_trail_max_points[active_view_idx()]; }
void trails_amount_set(int val) {
    g_config.view_trail_max_points[active_view_idx()] = val;
}

bool tag_id_shown() { return g_config.view_show_tag_id[active_view_idx()]; }
void tag_id_toggle() {
    int i = active_view_idx();
    g_config.view_show_tag_id[i] = !g_config.view_show_tag_id[i];
    storage_save_config(g_config);
}

bool tag_data_shown() { return g_config.view_show_tag_data[active_view_idx()]; }
void tag_data_toggle() {
    int i = active_view_idx();
    g_config.view_show_tag_data[i] = !g_config.view_show_tag_data[i];
    storage_save_config(g_config);
}

bool tag_type_shown() { return g_config.view_show_tag_type[active_view_idx()]; }
void tag_type_toggle() {
    int i = active_view_idx();
    g_config.view_show_tag_type[i] = !g_config.view_show_tag_type[i];
    storage_save_config(g_config);
}

bool secondary_locations_shown() { return g_config.view_show_secondary_locations[active_view_idx()]; }
void secondary_locations_toggle() {
    int i = active_view_idx();
    g_config.view_show_secondary_locations[i] = !g_config.view_show_secondary_locations[i];
    storage_save_config(g_config);
}
