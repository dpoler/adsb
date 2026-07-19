#pragma once

// Poll Serial (USB CDC) for one-line config commands, so long values (like an
// airportdb.io API token) can be pasted from a terminal instead of typed on
// the on-screen keyboard. Call once per loop() iteration — non-blocking.
//
// Supported commands (one per line, terminated by \n):
//   TOKEN=<airportdb.io api token>
void serial_config_poll();
