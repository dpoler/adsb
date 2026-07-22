#include "display_prefs.h"
#include "../data/storage.h"

bool tag_id_shown() { return g_config.show_tag_id; }
void tag_id_toggle() {
    g_config.show_tag_id = !g_config.show_tag_id;
    storage_save_config(g_config);
}

bool tag_data_shown() { return g_config.show_tag_data; }
void tag_data_toggle() {
    g_config.show_tag_data = !g_config.show_tag_data;
    storage_save_config(g_config);
}

bool tag_type_shown() { return g_config.show_tag_type; }
void tag_type_toggle() {
    g_config.show_tag_type = !g_config.show_tag_type;
    storage_save_config(g_config);
}

bool secondary_locations_shown() { return g_config.show_secondary_locations; }
void secondary_locations_toggle() {
    g_config.show_secondary_locations = !g_config.show_secondary_locations;
    storage_save_config(g_config);
}
