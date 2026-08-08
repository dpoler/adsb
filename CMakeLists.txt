cmake_minimum_required(VERSION 3.16)
project(FlightLevel314 LANGUAGES C CXX)

# FlightLevel314 — Raspberry Pi / Linux ADS-B display (forked from
# dpoler/adsb's Pi port). ESP32/jc1060 PlatformIO builds are not part of
# this project; historical ESP32 notes remain in docs/ for optional
# cherry-picks later.
add_subdirectory(pi)
