# AGENTS.md

## Cursor Cloud specific instructions

This repo is **FlightLevel314** — a Raspberry Pi / Linux ADS-B display
(CMake + LVGL SDL simulator or DRM). It was forked from `dpoler/adsb`'s Pi
port; the ESP32 / JC1060 PlatformIO target is **not** built here.

**Deeper project knowledge** (history, backlog, design decisions — including
archived ESP32 notes): see [`docs/project-knowledge.md`](docs/project-knowledge.md).
Treat it as point-in-time — verify against current code before relying on
specific "done" claims or file:line references.

### Toolchain
- Cloud env: `.cursor/environment.json` + `.cursor/Dockerfile` + `.cursor/install.sh`.
- Pi SDL simulator needs `cmake`, `libsdl2-dev`, `libcurl4-openssl-dev`.

### Build
```
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
cmake --build build -j$(nproc)
./build/pi/flightlevel314
```
Headless Cloud VMs can compile the SDL binary; interactive display may need
Computer Use / a display. Real Pi DRM (`-DPI_DISPLAY_BACKEND=DRM`) is out of
scope for this cloud environment.

### Lint / tests
No C++ unit tests and no lint config. CI builds the SDL binary on version
tags. Treat a clean CMake SDL build as the verification signal.

### Optional Python codegen (`tools/`)
Generate **gitignored** headers under `src/ui/`:
- `python3 tools/generate_airports_db.py` → `src/ui/airports_db.h`
- `python3 tools/generate_static_map.py --lat LAT --lon LON` →
  `src/ui/static_map_data.h`

### Standing preferences
- **User handles builds** unless asked. Prefer writing code over drive-by
  rebuilds.
- Do not reintroduce PlatformIO / jc1060 as a first-class target unless
  explicitly asked — cherry-picks back to `dpoler/adsb` are fine later.
- Config lives under `~/.config/flightlevel314/` (not `~/.config/adsb/`).
