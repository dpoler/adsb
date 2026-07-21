#include "display_prefs.h"

static bool _callsigns_hidden = false;

bool callsigns_hidden() { return _callsigns_hidden; }
void callsigns_hidden_toggle() { _callsigns_hidden = !_callsigns_hidden; }
