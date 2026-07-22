#include "display_prefs.h"
#include "../data/storage.h"

bool callsigns_hidden() { return g_config.hide_callsigns; }

void callsigns_hidden_toggle() {
    g_config.hide_callsigns = !g_config.hide_callsigns;
    storage_save_config(g_config);
}
