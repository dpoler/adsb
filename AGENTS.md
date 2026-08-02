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
  crash). It's pinned deliberately. `55.03.37` / ESP-Hosted `2.11.6` (host +
  C6 matched) is the only confirmed-stable pairing; `55.03.39` / `2.12.8` was
  tried matched and was worse.

### Standing preferences (from project memories)
These override default agent habits for this repo:
- **User handles builds.** Do **not** run `pio run` to verify firmware changes.
  Write the code and stop. Note compile concerns in text if needed; Dan builds
  on the board himself. (Exception: environment-setup / first-boot tooling
  checks may still run a build to prove the toolchain works.)
- **No Claude / AI attribution in commits.** Never add `Co-Authored-By: Claude`
  (or similar) trailers. Treat this as a hard checklist item on every commit.
- **Justify, don't default.** When carrying a tool/library/pattern into new
  work (new board, new platform, simulator, etc.), state why it fits *this*
  context's constraints — not just "it's already used elsewhere."
- **Prefer explicit commit permission** for ordinary interactive work. Make
  changes, then wait to be told to commit/push. Cloud Agent PR/setup workflows
  that require commits still apply when that is the assigned task.

### Hard architectural constraints (from project memories)
- **`jc1060` only.** CrowPanel / waveshare / S3 / CYD targets were removed;
  don't resurrect board-target scaffolding unless Dan asks.
- **No new FreeRTOS tasks for network work** on this board (permanent or
  one-shot). Piggyback on an existing task via request/poll/result; keep HTTP
  serialized through `http_mutex`. New task stacks compete with ESP-Hosted
  SDIO for scarce internal DRAM and have crashed with
  `assert failed: sdio_rx_get_buffer`.
- **Map ≠ Radar visibility.** Map intentionally draws/taps aircraft past the
  bullseye out to the rectangular canvas; Radar intentionally clips to a
  circle. Do not "fix" either to match the other.
- **No origin/destination / route display.** Removed on purpose; adsb.lol and
  adsbdb.com share unreliable VRS standing-data route tables. Don't bring it
  back.
- **LVGL delete-from-handler:** use `lv_obj_delete_async()` when deleting an
  object (or ancestor) from inside its own event handler.
- **Large stack locals off `loopTask`:** anything reachable from LVGL
  draw/timer callbacks runs on Arduino's ~8KB `loopTask` stack — big arrays
  must be `static`/heap, not stack-local.
