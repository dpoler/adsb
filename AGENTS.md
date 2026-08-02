# AGENTS.md

## Cursor Cloud specific instructions

This repo is **ESP32-P4 firmware** for the JC1060P470C board, built with
PlatformIO (`pioarduino` platform). There is no runtime that executes on the
cloud VM — the "application" is firmware that runs on physical hardware, so
"running" it here means a successful cross-compile/link into `firmware.bin`.
Flashing (`-t upload`) and `pio device monitor` require the physical board and
cannot be done in this environment.

### Toolchain / where things live
- `pio` (PlatformIO Core) is installed to `~/.local/bin`, which is added to
  `PATH` via `~/.bashrc`. If `pio` isn't found in a fresh non-interactive
  shell, run `export PATH="$HOME/.local/bin:$PATH"` (or invoke
  `~/.local/bin/pio` directly).
- First build downloads the pinned platform (`55.03.37`), the RISC-V toolchain,
  and libraries into `~/.platformio` (~several minutes). These are cached in the
  VM snapshot, so subsequent builds are fast (seconds).
- PlatformIO builds its own internal venv under `~/.platformio/penv`; this
  requires the `python3-venv` system package (already installed in the
  snapshot). If a build ever fails with "ensurepip is not available" / "Failed
  to create virtual environment", delete `~/.platformio/penv` and rebuild.

### Build (the primary "run" here)
- `pio run -e jc1060` — `jc1060` is the only supported target (also the default
  env). Success prints a memory usage table and `[SUCCESS]`.

### Lint / tests
- There are **no C++ unit tests** and **no lint config** in this repo. CI
  (`.github/workflows/release.yml`) only builds the firmware on tag pushes.
  `pio check` is not configured (no static-analysis tooling set up), so treat a
  clean `pio run -e jc1060` as the build/verification signal.

### Optional Python codegen tools (`tools/`)
These generate **gitignored** compiled-in headers under `src/ui/` (pulled in
via `__has_include`, so the firmware builds fine with or without them):
- `python3 tools/generate_airports_db.py` → `src/ui/airports_db.h` (downloads
  OurAirports CSV; no extra deps). Adds the static airport glyph DB; noticeably
  increases Flash usage when present.
- `python3 tools/generate_static_map.py --lat LAT --lon LON` →
  `src/ui/static_map_data.h` (needs `Pillow` + `requests`, already installed;
  downloads OSM tiles). Note: its `config.h` default path does not exist, so
  always pass `--lat`/`--lon`.

### Do not
- Do not bump the `platform =` version in `platformio.ini` (see the extensive
  comment there re: ESP-Hosted/C6 firmware pairing and the `sdio_rx_get_buffer`
  crash). It's pinned deliberately.
