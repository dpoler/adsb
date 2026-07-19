#pragma once
#include "aircraft.h"

// Connection type
enum NetType { NET_NONE, NET_ETHERNET, NET_WIFI };

// Initialize network and start the background fetch task
void fetcher_init(AircraftList *list);

// Returns true if any network is connected
bool fetcher_wifi_connected();

// Returns which network is currently active
NetType fetcher_connection_type();

// Returns the timestamp of the last successful fetch
uint32_t fetcher_last_update();

// On-demand point query for a non-home location (a saved airport far from
// home_lat/home_lon, which the main fetch loop never covers). Call with
// radius_nm > 0 to (re)target and start/continue polling; call with
// radius_nm <= 0 to pause polling and clear the list.
void fetcher_set_location_target(float lat, float lon, int radius_nm);
AircraftList* fetcher_location_list();

// Network stats
struct FetcherStats {
    uint32_t fetch_ok;
    uint32_t fetch_fail;
    uint32_t enrich_ok;
    uint32_t enrich_fail;
    uint32_t bytes_received;
    uint32_t last_fetch_ms;     // duration of last successful fetch
    char ip_addr[16];
};
const FetcherStats* fetcher_get_stats();
