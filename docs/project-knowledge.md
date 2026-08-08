# ADS-B Display Project — Full Knowledge Dump

> **Agent note:** Canonical cross-session knowledge for the *whole* ADS-B project
> (ESP32 `jc1060` + Raspberry Pi port), not only `pi-port`. Imported 2026-08-07
> from a Claude knowledge dump. Point-in-time observations — verify against
> current code before treating specific file:line / "done" claims as live.
> Day-to-day agent rules live in `AGENTS.md`; this file holds history, backlog,
> and deeper rationale.


Generated 2026-08-07 from accumulated cross-session memory. This is a point-in-time
snapshot — memory entries are point-in-time observations, not live state. Verify
against current code before treating any specific claim (file:line, function name,
"done" status) as still true.

Current git branch at time of writing: `pi-port` (clean working tree). Main branch: `master`.

---

## 1. What this project is

A touchscreen ADS-B aircraft display device. Originally a single ESP32-P4 board
target; now also being ported to a Raspberry Pi. Shows live aircraft traffic
(pulled from adsb.lol) on Map/Radar/List(Arrivals)/Stats views, with a
location-picker system (Home + saved airports/waypoints), filters, trails, alerts
for military/emergency squawks, and more.

---

## 2. Board targets (ESP32 side)

**Only one ESP32 board target exists today: `jc1060`** (JC1060P470C, ESP32-P4 +
ESP32-C6 WiFi co-processor over SDIO/ESP-Hosted, 1024x600 7" panel — "1060" is a
resolution-derived name, not a diagonal size). This is Dan's daily-use device.

Removed board targets, all deliberately, all at explicit user request:
- **`esp32s3_35`** (3.5" ESP32-S3, TFT_eSPI, had its own `src/ui_s3/` UI copy) —
  removed 2026-07-21, "I don't use it."
- **`cyd`** (bare standalone TFT_eSPI sketch, predates the LVGL `ui/`/`ui_s3`
  architecture) — removed 2026-07-24.
- **`waveshare`** (ESP32-**S3** board, Waveshare ESP32-S3-Touch-LCD-7B — not P4,
  correcting an earlier wrong note) — removed 2026-07-24.
- **`crowpanel`** (Elecrow CrowPanel Advanced 10.1" ESP32-P4) — added 2026-07-26
  after a full real-hardware bring-up (see §3 below), paused 2026-07-27 after an
  unsolved recurring blank-display bug survived three fix attempts, then fully
  **removed** 2026-07-28 at explicit request ("remove everything that isn't
  jc1060"). The bring-up work and the unsolved bug are kept as historical record
  in memory, not deleted, in case a similar board is revisited.

`platformio.ini` currently has exactly one env: `jc1060`.

**Convention note**: an earlier "ui/ vs ui_s3/ scoping" convention (shared
data-layer fixes go to both UI targets, new UI-only features only to `ui/`) is
now moot since there's no second ESP32 UI target. Don't assume any future board
should reuse the old `ui_s3/` approach — ask what it actually needs first.

---

## 3. CrowPanel bring-up (historical — board since removed)

Real-hardware bring-up done 2026-07-26 on branch `crowpanel-port`. Kept as
detailed history since the debugging trail (SDIO pin mapping, a real upstream
GT911 touch driver bug, DMA/cache-coherency investigation) is broadly relevant to
other P4-based work.

- **macOS USB driver**: WCH CH34x chip installs as a DriverKit system extension,
  not a legacy kext — enable via System Settings → General → Login Items &
  Extensions → Driver Extensions (no "blocked software" banner appears).
- **Upload speed**: esptool's default 1.5Mbps failed; settled on 230400 baud
  based on a matching GitHub issue from someone else on the same chip family
  (460800 "doesn't ack reliably").
- **SDIO/WiFi pins**: `sdkconfig.defaults.crowpanel` does NOT reach the build
  under `framework=arduino` (arduino-esp32 links a precompiled P4 lib with
  ESP-Hosted Kconfig baked in) — needed a runtime `hostedSetPins()` override in
  `fetcher_init()` instead. Elecrow's own official example gave wrong pin
  values (1-bit bus, D0=14/D1=15); the actually-correct values (4-bit bus,
  D0=17/D1=16/D2=15/D3=14, CLK=18, CMD=19, reset=32) came from a GitHub issue on
  the same repo where someone else reverse-engineered it under MicroPython.
  Lesson: vendor docs and vendor GitHub issues disagreed twice in the same repo
  — trust real hardware reports over official examples.
- **Touch — real upstream driver bug**: Espressif's own vendored
  `esp_lcd_touch_gt911.c` had a real bug (`espressif/esp-bsp#501`) —
  `get_xy()` unconditionally zeroed the touch-point count every call, causing
  false "released" reports mid-touch. Ported the upstream fix in two parts (the
  first part alone made it *worse* — touch latched permanently pressed).
  Software debounce in `gt911_touch.cpp` kept as belt-and-suspenders (3 tuning
  passes: V1 press-only didn't fix release-side chatter; V2 added release-side
  debounce but broke light taps; V3 = press-accept-immediately + 80ms
  post-release cooldown).
- **Two USB ports**: UART0 (WCH bridge, flashing/ROM banner) vs "USB2.0" (P4's
  native USB-OTG peripheral, what `Serial` actually uses under
  `ARDUINO_USB_MODE=0`). Two attempts to collapse to one cable both hung the
  board — root cause: ESP-IDF's console already owns UART0 from boot
  (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`), and `HardwareSerial.begin()` doesn't
  special-case an already-console-owned UART. **Decided: two cables is the
  permanent answer for this board.**
- **jc1060 build break found (not caused) by this bring-up**: PlatformIO
  compiles every `.cpp` under `src/` for every env by default — needed explicit
  `build_src_filter` exclusions per board for board-specific `hal/` files.
- **Blank-display bug — never root-caused, board removed instead**: recurring
  full-blank screen, backlight still on (ruled out backlight-only failure).
  Three independent, source-verified fixes all failed: `esp_cache_msync()`
  (redundant — driver already handles it), `num_fbs=2` alone (would cause
  tearing under LVGL partial-render mode, caught before flashing),
  `num_fbs=2`+`RENDER_MODE_FULL` (structurally correct, still didn't fix it).
  New evidence pointed at a possible shared power/electrical fault (a
  simultaneous I2C touch-bus failure burst was found in a separate UART0 log).
  Paused 2026-07-27 per user decision, then the whole board was **removed**
  2026-07-28.

---

## 4. Platform version pin history (jc1060, ESP32-P4/C6)

`platformio.ini` pins `pioarduino`/`platform-espressif32` to **`55.03.37`**
(Arduino-ESP32 3.3.7) for `jc1060`. This is a hard-won, confirmed-stable value —
**do not bump without new evidence**.

History:
- Started at `53.03.13` in the initial scaffold, bumped to `55.03.37` early on.
- Both `jc1060` and `waveshare` (now-removed) were bumped together to `55.03.39`
  on 2026-07-18 for a WiFi crash fix scoped to `waveshare` — but this silently
  raised jc1060's expected ESP-Hosted protocol version (2.11.6 → 2.12.8),
  reopening a host/C6 co-processor version mismatch that had just been fixed via
  `dpoler/c6_updater`.
- Reverted 2026-07-21 (`jc1060` back to `55.03.37`), confirmed via new boot-time
  version logging: `host v2.11.6, C6 co-processor v2.11.6`.
- **Re-attempted and reverted again same-day, 2026-07-24**: user recalled the
  newer platform being "much faster" for WiFi bring-up and wanted to try
  matching both sides at the newer version instead of pinning backward forever.
  Retargeted `c6_updater` to 2.12.8, bumped `jc1060` to `55.03.39`. Result: WiFi
  connected fast (~1s) with no mismatch — but then crashed hard with
  `assert failed: sdio_rx_get_buffer` within ~10s of every boot. **Conclusion:
  matching host/C6 versions does NOT fix the underlying open upstream
  DMA-fragmentation bug (espressif/esp-hosted-mcu #144/#167) — it only removes
  mismatch as a separate contributing factor. 2.12.8 triggers the remaining bug
  far more severely than 2.11.6 on this board.** Fully reverted same day.
  **Hard rule: do not retarget to 2.12.8 (or assume "newer = better") without
  new evidence the upstream DMA-fragmentation bug is fixed.**
- Same risk resurfaced on CrowPanel (factory C6 firmware was "V2.12.3") —
  deliberately pinned to the same 2.11.6 host to avoid the crash, accepting a
  guaranteed version mismatch instead (never got to test which was worse before
  the board was removed for the unrelated blank-display bug).

---

## 5. ESP32-P4 heap / DMA constraints (jc1060)

The board runs thin on internal DRAM (PSRAM is abundant, 32MB, but not what task
stacks/TLS buffers/SDIO driver allocations use).

- **Never spawn new FreeRTOS tasks for network work** — even a short-lived
  one-shot task (8KB stack) for a single HTTPS request has crashed the SDIO
  driver with `assert failed: sdio_rx_get_buffer sdio_drv.c:953 (*buf)`.
  Piggyback new fetch logic onto an *existing* task's loop instead (the
  established request/poll/result pattern — see `locations_request_add()`/
  `locations_add_poll()`/`locations_add_result()`).
- **Root cause identified as upstream, not our task-spawn pattern**: an
  Espressif engineer confirmed the real mechanism is DMA-capable memory
  fragmentation (not just low free heap) — `espressif/esp-hosted-mcu` issues
  #144/#167, still open, hit by many unrelated projects/boards. Not fixable
  from this project's application code or build setup.
- **C6 co-processor firmware was stale (2.3.2), updated to 2.11.6** via
  `dpoler/c6_updater` (the user's own tool, uses Arduino-ESP32's official
  `ESP_HostedOTA` mechanism over the existing SDIO link). Crash persisted even
  after the update, confirming it's the still-open upstream bug, not a stale-C6
  problem, at 2.11.6.
- **Fixed, separate bug: NetworkClientSecure's 120s handshake timeout wasn't
  bounded by `HTTPClient::setTimeout()`** — caused ~150s silent stalls (mutex
  held the whole time, starving every other network consumer). Fixed at all 6
  active HTTPS call sites by constructing `WiFiClientSecure` explicitly and
  calling `.setHandshakeTimeout(8)` before `HTTPClient::begin(client, url)`.
- **Packet-mode mitigation (upstream docs) is out of reach**: requires
  `idf.py menuconfig` on both P4 host and C6 co-processor sides; this project
  has neither a checked-in sdkconfig nor builds/flashes the C6's own firmware.
  Not attempted.
- **LVGL object deletion from inside its own/a descendant's event handler is
  undefined behavior** — fixed by using `lv_obj_delete_async()` instead of
  `lv_obj_delete()` (LVGL's own recommended pattern, used by `lv_msgbox`
  internally). Applies to any future LVGL popover/modal code.
- **Large blocking NVS writes visibly stall the LCD panel** — a ~3.2KB write
  caused a visible cyan flash; an ~400 byte write (Settings) did not. Fix:
  write only what changed/is used, not fixed-size worst-case buffers.
- **Large stack-local variables in the LVGL render path crash `loopTask`**
  (only ~8KB stack, separate/smaller than background task stacks) — a ~3.75KB
  struct declared as a plain stack-local inside a draw callback caused a
  deterministic stack-canary panic. Fix: make it `static` instead.

---

## 6. Raspberry Pi port (branch `pi-port`)

**Goal**: fork this ESP32-P4 ADS-B display to also run on a Raspberry Pi 3B+
with a Waveshare 10.1" DSI capacitive touch panel (1280x800, resolves an earlier
open "is it 3B or 3B+" question — confirmed 3B+ once real hardware arrived),
to unlock functionality the ESP32 fundamentally can't do (real aircraft photos,
richer maps/charts, more compute). Architecture plan doc:
`/Users/dap/.claude/plans/misty-exploring-cook.md` (may be pruned over time).

### Key architecture decisions
- **Data source**: stays on remote adsb.lol for now, same as ESP32. An
  `AircraftDataSource` interface (`src/data/datasource.h`) is stubbed so a
  future local RTL-SDR + dump1090/readsb feed is a new class
  (`LocalSdrDataSource`, not yet implemented), not a fetcher rewrite.
- **Code sharing**: shared core + a platform-abstraction layer
  (`src/platform/platform.h`: mutex, monotonic time, HTTP GET, config storage,
  log). Monorepo, not a separate repo. Most of `src/ui/` compiles for both
  jc1060 (PlatformIO/Arduino) and Pi (CMake/g++) with minimal per-file changes.
- **Layout constraint**: everything Pi/Linux-only lives in a top-level `pi/`
  directory, outside `src/` (PlatformIO auto-compiles everything under `src/`,
  so Linux-only headers there would break the jc1060 build).
- **OS**: Raspberry Pi OS (came pre-installed with desktop; desktop was
  **disabled**, not reinstalled to Lite — `systemctl set-default
  multi-user.target` + disable lightdm, reversible). LVGL directly over DRM/KMS,
  running as a systemd kiosk service (`pi/adsb-pi.service`).
- **Mac dev loop**: LVGL's SDL2 simulator (fast UI iteration) plus an aarch64 Pi
  OS VM via UTM/QEMU for system-integration testing (no real GPU passthrough,
  so not for pixel-exact display testing).
- **LVGL was justified, not defaulted to**: user explicitly required a real
  justification for carrying LVGL forward rather than assuming it because it's
  already used — confirmed it's the right fit given the Pi 3B's weak GPU and
  this app's canvas-heavy custom rendering (radar sweep, phosphor fade,
  split-flap board), and it's what keeps the UI layer shareable with ESP32.
- **`src/data/fetcher.cpp` (jc1060's WiFi/C6 fetch loop) deliberately left
  untouched** — its hardware-recovery logic is hard-won (see §4/§5). Pi instead
  has a fresh reimplementation, `pi/platform_linux/datasource_remote.cpp`
  (same adsb.lol JSON schema, independent code — a known drift risk if the
  schema changes, accepted).
- **No SSH access from Claude** to the Pi — all hardware bring-up work happens
  by handing the user exact copy-paste command bundles; standing operating mode
  for all `pi/` hardware work.

### Status as of 2026-08-06 (most recent)
Real Pi 3B+ hardware arrived 2026-08-06. **Task #6 (real hardware bring-up)
closed out** — DRM display, real touch, real network fetch, and surviving an
unattended reboot are all verified on actual hardware (not just SDL simulator).

Two real build bugs found and fixed getting the DRM build running:
1. `pi/CMakeLists.txt` set nonexistent LVGL v9.5.0 CMake options
   (`LV_CONF_BUILD_DISABLE_EXAMPLES/_DEMOS`) — silently a no-op, harmless on
   SDL. Real option names are `CONFIG_LV_BUILD_EXAMPLES`/`_DEMOS`.
2. `${DRM_INCLUDE_DIRS}`/`${LIBINPUT_INCLUDE_DIRS}` were only added to the
   `adsb_pi` target, not the `lvgl` target itself — but `LV_USE_LINUX_DRM`
   makes `lvgl.h` itself pull in `<drm.h>` for every LVGL source file. Debian
   ships `drm.h` under `/usr/include/libdrm/`. Fixed by adding those include
   dirs with `PUBLIC` scope to the `lvgl` target too.
3. Separate bug: several shared files call `snprintf` without `#include
   <cstdio>`, relying on transitive inclusion — true on macOS/ESP32 Arduino,
   not true on Debian/GCC 14. Fixed the four call sites actually in the Pi
   build's `SHARED_SOURCES` list; left the same latent bug alone in
   ESP32-only/Pi-stubbed files (out of scope).

`display_drm.cpp`/`input_libinput.cpp` needed **zero changes** — both
previously-unverified hardware assumptions (`/dev/dri/card0`, capability-based
Goodix touch auto-detection) were correct on the first real run.

**Kiosk service installed and confirmed surviving an unattended reboot** — fix
was pinning `Environment=HOME=/opt/adsb-pi` in the systemd unit and
seeding/chowning `/opt/adsb-pi/.config/adsb` from the already-tested config
before first launch.

**Real perf constraint found on Pi's DRM rendering path**: each single
`lv_draw_line`/`lv_draw_rect` call costs roughly **4.5–6ms**, flat, regardless
of pixels touched (confirmed via real per-phase profiling, not guessing) —
likely because `pi/lv_conf.h`'s `LV_DRAW_SW_DRAW_UNIT_CNT=1` means
single-threaded software rasterization, not using the Pi's other cores (not
root-caused further). **Practical rule for future Pi rendering work: budget
draw calls tightly** — at ~5ms/call, 20-30 extra draw calls can blow a 100ms
(10fps) frame budget. `draw_rings()`'s ~10-12 calls already cost ~27ms baseline.

**Display-sizing pass closed out (commits 03cc328, bbaaa32, db58560)**: List's
`MAX_ROWS` (was a jc1060-tuned literal 15) now derives from `BOARD_H`; confirmed
21 rows on Pi matching the formula. Map/Radar bullseye center/radius
(`MAP_BULLSEYE_CY/R`, `RADAR_CY/R`) are per-screen `#if LCD_V_RES == 800`
measured constants (via a real ruler+photo measurement pass, same method as the
original jc1060 bullseye-centering saga), jc1060's values kept byte-for-byte
unchanged in `#else`. User confirmed "good for now."

### What's ported and working on Pi (as of 2026-08-06)
Platform seam, config storage, real adsb.lol fetch, Map/Radar/Arrivals/Stats
views, detail_card, filters, display_prefs, range, status_bar (nav tabs/gear/
range chip), view_menu (VIEW chip popover — trails/tags/secondary-locations/
alert toggles), alerts.cpp (military/emergency toasts — plumbing verified,
toast animation itself not yet visually exercised), a real saved-locations
system (fresh Linux `locations_linux.cpp` implementation + ported
`location_picker.cpp` UI), systemd kiosk service, README.

### Still stubbed (`pi/app_stubs.cpp`)
`metar.cpp`, `airlines.cpp`, `enrichment.cpp` — all three network-backed
(adsbdb.com/planespotters.net), independently portable, none block each other.
`enrichment.cpp` is flagged as a genuine Pi-exclusive opportunity: jc1060 can't
render fetched photos at all due to a PSRAM cache-coherency erratum (README
known-issue), but the Pi has no such constraint — real aircraft photos in the
detail card. See backlog §7 for full scope.

### Notable bugs found and fixed during the port (worth remembering)
- **`platform.h` must `#include <Arduino.h>` itself under `#if defined(ARDUINO)`**
  — a file switched from directly including Arduino.h to including platform.h
  can silently lose `millis()` on the ESP32 build if platform.h doesn't
  re-export it. Cost one failed jc1060 build to catch (user builds jc1060
  themselves per [[feedback_builds]] — this class of bug only surfaces on
  their side).
- **Tileview scroll-range bug**: `lv_tileview_add_tile()` positions tiles with
  `lv_pct()`, only resolved into real pixels during a layout pass — the
  scrollable-width calc got stuck against whatever was resolved at that moment.
  Fixed with an explicit `lv_obj_update_layout()` call right after
  `views_init()` in `pi/main.cpp` (jc1060 never hit this because it creates
  enough other widgets between init and first frame to force the recompute
  incidentally).
- **SDL mouse driver incompatible with LVGL's scroll-momentum math**
  (`LV_INDEV_MODE_EVENT` + many events in the same observable millisecond →
  velocity reads as near-infinite → tileview flings to the far edge — LVGL
  issue #6832). Fixed by writing a custom POLL-mode SDL input driver
  (`pi/input_sdl.cpp`) instead of using LVGL's bundled `lv_sdl_mouse_create()`.
  Pi-simulator-only, doesn't affect real hardware (`input_libinput.cpp` is a
  separate driver).
- **The actual decisive root cause of the "swipe jumps to extremes" bug**: a
  separate, **shared-code** manual swipe detector in `views.cpp`
  (`views_attach_swipe()`) used `% NUM_VIEWS` which wraps (Map→Stats), but the
  tileview's actual layout is a straight line, not a loop — wrapping jumped
  straight across with no animation. User chose **clamp at the ends** over
  keeping wraparound. This was shared code, not Pi-specific, so needed a jc1060
  build check. **Process lesson**: several earlier plausible-but-wrong theories
  (tile position, input driver) were each real and partially confirmed but not
  the actual cause — once local instrumentation cleanly isolates a symptom,
  check *all* code paths that could react to that exact trigger, not just the
  one already under suspicion.
- **Behavior change, intentional**: Pi's first boot now starts with **zero**
  saved locations (matches ESP32's real first-boot state), not the old
  hardcoded-KSEA stub. No aircraft show until a location is added via the
  picker.

### Remaining known gaps (not blocking, tracked in backlog)
- Boot sequence isn't tidy — console text/login prompt likely visible before
  the kiosk grabs the display.
- No on-device way to set the airportdb.io token on Pi (worked around once
  with a one-off Python script hand-editing the config JSON).

---

## 7. Full backlog (as of 2026-08-08)

This preserves essentially the full detail of every backlog entry — done items
are kept for their debugging trail/rationale (useful history), open items are
what's actually outstanding. Grouped roughly by theme; original memory file is
mostly chronological.

### 7.1 Genuinely open / not started

- **Detail card photo credit appears before the photo**: photographer credit
  text can pop into the summary/detail area before the aircraft photo has
  finished loading (or when the image path fails / is still decoding). Credit
  should stay hidden until pixels are actually shown, or sit only under the
  photo slot. **Do not start until explicitly asked** — reported 2026-08-08.

- **Location switch: successful fetch but empty Map for a couple of refreshes**:
  switching active location from Heathrow (EGLL) to Denver, `adsb.lol`
  appeared to fetch successfully but no aircraft rendered for a couple of
  refresh cycles afterward. Possible stale list / range filter / projection
  center race, or a brief empty payload accepted as OK before the new
  location's traffic arrives. Repro and root-cause not done. **Do not start
  until explicitly asked** — reported 2026-08-08.

- **Basemap / sectional coverage outside the US (esp. UK)**: FAA VFR
  sectional style is US-charting only — expected empty/useless for UK and
  other non-US regions; either gate the style by geography or label it
  US-only in the VIEW menu. Separately, at EGLL (~51.47N) with dark_nolabels
  @ 10 nm the basemap worker aborts: `tile AABB too large (25x18 at z=13)`
  (`pi/basemap.cpp` guard `tiles_w * tiles_h > 300`) after cache TTL expiry,
  so the map never rebuilds. Mercator AABB vs equirectangular canvas grows
  with latitude; need a zoom/AABB fix and a pass verifying **all** basemap
  styles at representative worldwide locations (low / mid / high lat, both
  hemispheres). **Do not start until explicitly asked** — reported
  2026-08-08.

- **Include small airports in the static on-device airport DB**: today's
  `tools/generate_airports_db.py` keeps only OurAirports `large_airport` +
  `medium_airport` (~5k entries, ~0.4 MB const with `name[64]`). Adding
  `small_airport` (ident ≤4, same filter) is ~+25.6k rows → ~30.6k total and
  ~2.5 MB aligned const flash (~+2.1 MB). Header text scales similarly
  (~0.35 → ~2.1 MB). Fine on Pi; painful if the same table stays shared with
  ESP32. **Do not start until explicitly asked** — sized 2026-08-08.

- **Fork the Pi port into its own project/repo**: Pi-specific surface area
  (basemap/weather, SDL/DRM, AeroDataBox O/D, AirportDB, settings, photo
  path, CMake) has diverged enough that sharing `src/ui` + `src/data` with
  the jc1060 ESP32 tree is getting costly. Plan a clean fork (or extract)
  so Pi can evolve without `#if !defined(ARDUINO)` / dual-target friction.
  **Do not start until explicitly asked** — parked 2026-08-08.

- **Pi online app updates (check / notify / pull / restart)**: periodically
  check whether a newer `adsb_pi` (or package) is available, surface a
  non-intrusive "update available" notice in the UI, download it, and restart
  into the new build. Not designed — open questions include update source
  (GitHub Releases vs self-hosted URL vs apt), signature/verification,
  whether the kiosk systemd unit should own the swap, and how aggressive the
  check cadence should be on a wall-mounted always-on display. Related to the
  Settings "Device" column / "Check for Update" idea below, but that entry was
  framed around ESP32 OTA; this is the Pi-native equivalent. **Do not start
  until explicitly asked** — parked 2026-08-08 so it isn't forgotten.

- **planespotters.net photo fetch — dead on jc1060, revive for Pi**: PSRAM
  cache-coherency erratum on jc1060 corrupts image data (a genuine hardware
  blocker), so this was originally slated for removal — **reversed 2026-07-31**,
  user wants real aircraft photos on the **Pi port** specifically, since Pi has
  no such constraint. Not started: needs an image decoder/HTTP-image-fetch path
  (`LV_USE_LODEPNG` is already enabled in both `lv_conf.h`s but nothing fetches
  +decodes a remote JPEG/PNG into an `lv_img` buffer yet), and `detail_card.cpp`
  (shared code) needs an actual image widget, gated so jc1060 keeps its current
  text-only credit line. Worth checking `photo_url`'s actual image
  format/size from a live response before designing the decode path.

- **Ground traffic (GND) should default to hidden, not shown**: flip
  `storage.cpp`'s `cfg.view_hide_ground[i]` default false→true. Check whether
  existing saved installs (which already have an explicit NVS value) should be
  migrated — probably not, this only affects a fresh factory-reset device.

- **Need a way to view/edit/rename saved locations, not just add/remove**:
  motivating case — a sign-flipped-longitude typo (Inner Mongolia instead of
  Denver) currently can only be fixed by delete-and-re-add, with no way to even
  glance at a saved location's actual lat/lon to notice something's wrong.
  Needs a details/edit view (on-device + CLI script), with airport-type
  locations (ICAO-sourced) possibly needing different edit semantics than plain
  waypoints (not designed).

- **WiFi-only on-screen/script messaging needs to account for Ethernet**: the
  "No WiFi configured" overlay is gated to suppress correctly when Ethernet is
  already on, but a factory-reset device defaults to WiFi mode, so it shows a
  WiFi-only message even for someone planning to use Ethernet. The setup
  scripts have no "I'm using Ethernet" branch and no serial command to set
  `use_ethernet` at all. Needs an `ETHERNET=` serial command + wizard branch +
  overlay wording covering both options.

- **Generalize "Home" into the saved-locations system (remove the Home/
  saved-airport split)**: user wants Home eliminated as an architecturally
  distinct concept — one unified list (suggested cap ~8, not final) where each
  slot is either an airport or a plain lat/lon waypoint. Needs a discriminator
  field on `Location` (can't reuse `runway_count==0`, that already means
  "fetch pending" for airports). Every current Home special-case needs
  collapsing into the general path. NVS migration undecided (fold into slot 0,
  or drop and let user re-add). UI needs a second add-mode (manual lat/lon
  entry, replacing the Settings home-lat/lon fields).
  *(Note: memory says "Done, per user confirmation 2026-07-26" in one place but
  the detailed body describes it as not-yet-implemented design work — treat as
  unclear/needs a fresh check against current code before assuming either
  status.)*

- **Departure/destination via airframes.io ACARS**: deferred. Static
  callsign→route tables (adsbdb, adsb.lol) are unreliable/stale/non-directional.
  Better source: airframes.io OOOI events (actual departure/destination
  telemetry from the aircraft), but free tier requires running an ACARS feeder
  (acarsdec/dumpvdl2) on the same SDR hardware. Commercial-jets-only coverage.
  Revisit if/when an ACARS feeder exists.

- **Follow Mode — track a single flight as it travels**: select an aircraft and
  have Map/Radar re-center on it continuously. Main open design question: the
  underlying ADS-B query is a fixed-radius fetch around the active location —
  a followed aircraft flying away will eventually exceed the query radius and
  vanish. Needs an answer (re-center the query too? accept follow ends at query
  edge?), a clear exit path, and probably a status-bar indicator.

- **Airport Mode, Phase 3 (airport info panel — METAR/ATIS/frequencies)**:
  the only remaining piece of an originally 3-phase idea (Phases 1/2 turned out
  to already be covered by the general location system). Would need live METAR
  (aviationweather.gov/checkwx.com), COM/ATIS/CTAF/ground frequencies
  (OurAirports `airport-frequencies.csv`, ~300KB), active-runway-from-wind,
  D-ATIS text via `datis.clowd.io` for major US/European fields. Needs its own
  panel on the existing saved-airport UI.

- **Quality of life / display settings**: color themes, font size — not
  started. Brightness backend is real/complete but has no working UI on either
  board (see Screensaver entry below — the only exposing UI is disabled).

- **Screensaver / sleep mode (brightness control included)**: built once
  (commit cf531b2: independent dim/blank idle timers, brightness slider, a
  drifting/jumping aircraft-count screensaver), then deliberately deactivated
  (`#if 0` in `screensaver.cpp`, both board targets) after user questioned the
  motivating burn-in rationale (this is an LCD panel, not OLED — burn-in
  doesn't apply the same way; the closest LCD analog, "image persistence,"
  fades on its own). Open question if revisited: pure inactivity-based
  dim/blank doesn't fit a rarely-touched wall-mounted "picture frame" use case
  — might want a time-of-day schedule instead/in addition, which would need
  NTP/RTC wall-clock time from scratch (nothing in this codebase currently has
  real time-of-day, only `millis()`-based elapsed time). Not decided — deferred
  for a later conversation.

- **Alert beeper**: decided against — no buzzer on the jc1060 board, would need
  external piezo/speaker + GPIO + `ledc` PWM. If hardware is ever added: short
  beep on watchlist, escalating tone on military, urgent pattern on emergency.

- **Redesign `configure_device.sh`/`.ps1`'s UX**: functionality confirmed
  working end-to-end; presentation is "really ugly" per the user. No specific
  redesign direction given yet — ask for specifics (menu layout? colored
  output? progress indication?) before implementing.

- **Rationalize View/Filter UI — FILTER menu next to VIEW**: **built, then
  fully reverted the same day (2026-07-28)**. User's reaction after seeing it
  working: the always-visible button *column* gave a "real radar display,
  instrument panel" feel that a collapsed "FILTER: X + Y" text line lost
  entirely, even though it was more compact/discreet as literally requested.
  **Lesson for next attempt**: "more discreet" and "keep the instrument-panel
  look" are in tension and weren't surfaced as a tradeoff before building —
  ask up front next time. Three alternatives were offered (always-visible
  non-interactive dot strip, active-only colored chips, full revert); user
  chose full revert, meaning the visual presence of the button column is
  apparently a feature in itself, not just clutter to minimize.

- **Add a "Device" column to Settings; move VERSION off Stats**: Stats' NETWORK
  column has a VERSION row as a stopgap, but Stats is about live telemetry, not
  device identity. Plan: a second Settings column (single-column since commit
  2c09307) for VERSION + "Check for Update" button + potentially AIRPORTDB
  status (currently also on Stats). Not designed — exact contents/whether this
  revives the two-column layout is open.

- **No way to set the airportdb.io token on a Pi device**: see §6.

- **Tidy up the Pi's boot sequence**: see §6.

- **Pi-exclusive: real maps/sector charts using the extra resource budget**:
  raster/vector basemap tiles, FAA VFR sectionals. `tile_cache.cpp` exists in
  `src/ui/` but is explicitly disabled on ESP32 ("tiles broken on ESP32-P4") —
  worth checking if it's closer to reusable on Pi than starting fresh.
  Licensing/sourcing/storage-budget for sectionals not investigated. Not
  scoped. (Partial: Pi Map now has live/cached basemap styles via
  `pi/basemap.cpp` — Carto dark / dark_nolabels / Voyager cream light /
  voyager_nolabels / OpenTopoMap / FAA VFR sectional — with per-style
  disk-cache TTLs and a Settings "Clear map cache" button; see PR #4.)

- **Map legend backdrop vs basemap (Pi)**: before the basemap, the opaque
  legend panel (`draw_legend_backdrop` in `map_view.cpp`) was invisible
  against the solid `#0a0a1a` canvas. With tiles under Map it reads as a
  solid bar over geography. Prefer making that backdrop transparent (or
  much lower opacity) so coastlines show through; if that hurts label
  readability, bump Map bullseye center up a little instead (`MAP_BULLSEYE_CY`
  on the 800px Pi path) so the rings clear the legend. Noted 2026-08-07.

- **Basemap vs runway/aircraft alignment (Pi)**: ~~Mercator tiles blitted
  1:1 vs equirectangular `MapProjection`~~ — addressed 2026-08-07: basemap
  build now warps tiles into the MapProjection frame (`eq1` cache key in
  `pi/basemap.cpp`). Residual mismatch can still come from OSM/FAA chart
  artwork vs airportdb runway endpoint definitions (different datasets).

- **Radar sweep arm — make it smoother/more "radar-like"**: wants improved
  motion, implies something like a fading trail behind the sweep line. Not
  scoped yet — current implementation not re-examined against this request.
  (Note: separately, real Pi profiling — see §6 — found each draw call costs
  ~5ms flat on the Pi's DRM backend; any redesign here needs to budget draw
  calls tightly on that platform.)

- **Origin/destination display — user has a new approach, revisit**: feature
  was fully removed (§8, route data) due to unreliable VRS-sourced data. User
  says they've since "figured out a way to deal with" the accuracy problem —
  no details given yet. Treat the original critique as still valid until the
  new approach is explained; don't assume it's solved.

- **Flight following / tracking mode**: new idea, name only, no detail —
  possibly the same as "Follow Mode" above, possibly distinct. Needs scoping
  with the user before design starts.

- **Tap-to-open ATIS overlay on the INFO/Stats screen**: follow-on to the
  already-shipped METAR readout. Scoped to US airports first. Known complexity:
  some airports (KDEN named specifically) publish separate arrival/departure
  ATIS, not one combined broadcast — overlay needs to show both distinctly.
  No ATIS data source identified/vetted yet.

- **Logging cleanup — broader scope**: the concrete gap that motivated this
  (enrichment.cpp silent failures) is fixed, but ~31 `Serial.print*` call sites
  across the codebase still have no log-level system, no consistent
  prefix/format, mixed ad hoc debug prints and durable status logs. Needs
  clarification on the actual goal before a full pass.

- **README.md needs a massive update**: badly stale (still describes the old
  single-location architecture, lists removed route/origin-destination
  fields, lists removed FAST/SLOW/ODD filters). Must include a documented
  known-issues section on the SDIO crash (confirmed open upstream bug, not
  fixable here) and the full WiFi/platform-version-pin saga (host/C6 matching
  requirement, the confirmed-stable 55.03.37/2.11.6 pairing vs. confirmed-bad
  55.03.39/2.12.8, `dpoler/c6_updater`, the fast-fail/flat-delay WiFi fixes,
  and that this is a two-repo story).

- **Configurable poll/refresh interval in settings**: *(deprioritized
  2026-07-21, not current priority)* fetch interval is hardcoded 20000ms in
  `fetcher.cpp`. Would need a `poll_interval_s` field (default 20s, range
  5-120s), NVS persistence, a settings slider, and the fetcher loop + stats
  view timer reading from it.

### 7.2 Notable "done" items worth knowing about (bugs, root causes, decisions)

*(Kept because the debugging trail/rationale is broadly useful — e.g. don't
re-introduce these exact bugs, or don't second-guess these exact decisions
without new evidence.)*

- **Map vs Radar visibility rules are intentionally different** (see §9 below)
  — do not "fix" one to match the other.
- **WiFi attempt-1-wastes-30s / fast-fail / C6-reset-readiness saga**: multiple
  rounds — root-caused that `wifi_connect_with_timeout()` never inspected
  `WiFi.status()` until its own timeout fired; added fast-fail on
  `WL_CONNECT_FAILED`/`WL_NO_SSID_AVAIL`. This then **exposed a second bug**:
  `reset_wifi_c6()`'s "readiness wait" polled for a status transition
  (255→other) that could never actually be observed (real status was 254 from
  a fresh boot) — it had always been a flat ~1.2s delay dressed up as adaptive.
  Fixed to an honest flat 3s delay. A regression was found and fixed after
  that: back-to-back fast-fail retries at 0-1ms intervals crashed the SDIO
  transport — added a 1s inter-retry delay.
- **Bullseye/legend centering saga (jc1060)**: four wrong guesses in a row
  before the team added a labeled 50px debug ruler overlay + forced the
  tileview scrollbar always-visible, then had the user photograph real
  hardware and read exact numbers off it. **Lesson: don't keep guessing from
  source alone for "where exactly does X render" questions — add a measurement
  ruler and ask for a photo immediately.** (This exact method was reused
  successfully for the Pi's bullseye sizing — see §6.)
- **"Waiting for aircraft" overlay stuck forever with 0 err shown**: root cause
  was a completely separate, unmonitored fetch path (`location_fetch_poll()`
  for saved/non-Home locations) with no stats tracking and no error logging.
  Fixed with a second stats counter + error logging. A real repro then
  surfaced: a saved location with a sign-flipped longitude landed in Inner
  Mongolia (zero real coverage there) — confirmed not a bug, a data-entry
  mistake, but exposed a genuine UX gap: the overlay still can't distinguish
  "genuinely empty feed" from "still connecting" or "broken." Not implemented:
  dismiss-after-N-empty-fetches or an explicit "connected, no traffic" state.
- **VIEW menu (trails, tag fields, secondary-locations)**: replaced separate
  TRAIL/TAG status-bar chips with one popover. Went through several follow-up
  fixes: cross-view setting leak when switching views with the popover open
  (fixed by closing the popover on any view change), a `lv_switch` resize bug
  that made "Show trails" unresponsive (root cause: an explicit
  `lv_obj_set_size()` after creation desynced the switch's hit-test region from
  its visual layout — two earlier "fixes" that replaced the switch with custom
  pill widgets were reverted once this was found), and per-view (Map vs Radar
  vs Arrivals) settings for trails/tags/secondary-locations/filters/GND, all
  stored as small `[N]`-indexed arrays.
- **Per-location "show nearby large airports' runways" toggle**: narrowed from
  "all airports in radius" to large-only after a sizing discussion (KJFK at
  50nm has ~20 airports). Only the active location's nearby-cache is kept
  resident in DRAM (lazy-loaded, avoiding a ~15x DRAM multiply).
- **VERT/GND mutual exclusion, GND illumination convention flip,
  HIGH/LOW filters, multi-select filter AND/OR semantics** — all done, with the
  general rule: category filters (COM/GA/HELI/MIL/EMG) are OR'd together
  (alternative classifications), state filters (VERT/HIGH/LOW) are AND'd
  against the category selection (narrowing conditions), and GND is a separate
  unconditional exclude, not part of the bitmask at all.
- **adsb.lol 429 rate-limiting backoff**: added `Retry-After`-aware and
  exponential (capped 5min) backoff on both pollers, after a user-reported
  ~13% error rate.
- **KORD showed decommissioned runways / missing active ones**: root-caused as
  airportdb.io's own upstream data being stale (confirmed against the live
  OurAirports CSV mirror), not a parsing bug in this project — user reported it
  directly to airportdb.io.
- **Cyan-flash-on-delete, and the general NVS-write-size lesson**: see §5.
- **Location picker LVGL delete-from-event-handler bug**: see §5's
  `lv_obj_delete_async()` note — the same underlying issue recurred/was fixed
  across multiple UI files (location picker, trail menu).
- **OTA updates**: full application-firmware OTA via GitHub Releases, built
  v0.1.0→v0.1.4 in one evening (see §10 below for full detail).
- **Resume last-used view/radius/location/filters after reboot**: persisted
  from discrete human-paced actions only (never from the since-removed
  auto-cycle timer, to avoid frequent blocking NVS writes). Location persisted
  by ICAO string (not array index, which isn't stable across removes). Needed
  two follow-up fixes: a boot crash when resuming into Arrivals with a
  non-Home location active (a mutex wasn't initialized yet at the point the
  resume call ran), and Map's WiFi-connecting overlay being invisible when
  boot resumed into a non-Map view (it was parented to the Map tile
  specifically).
- **Auto-cycle-views feature removed entirely** (2026-07-28) — if ever wanted
  back, needs to be rebuilt from scratch; nothing was left half-wired.
- **Settings panel progressively trimmed to a single column** — WiFi/Ethernet/
  Range/Metric only; Auto-Cycle, Home lat/lon, GND, Trails, and the
  airportdb.io token field were all removed from Settings over several passes
  (moved to VIEW menu, promoted to filter-column buttons, or removed entirely
  in the token's case — "we're not going to type it in here").
- **Small airports deliberately excluded from the static DB** — an accepted,
  permanent size/perf tradeoff (~42,700 more airports, ~683KB, ~8x scan cost),
  not a bug to revisit casually.
- **Better airportdb.io token entry mechanism**: replaced ad hoc manual serial
  typing with a structured `OK `/`ERR `-prefixed line protocol
  (`serial_config.cpp`) plus cross-platform CLI scripts
  (`configure_device.sh`/`.ps1`) — grew from "just the token" into a general
  low-friction config channel (WiFi credentials, factory reset, a first-time
  setup wizard). Two real protocol bugs found and fixed during testing: a
  `set -e`-triggered silent script death on read-timeout, and a serial-line
  desync where an unrelated debug print line got read as the real command
  reply, permanently offsetting every subsequent read by one command.

---

## 8. Route/origin-destination data (removed feature)

**Status: removed entirely from the app** (`Aircraft.origin`/`.dest`, the
background enrichment task, the Arrivals ROUTE column, Radar's route display,
detail card route labels). Root cause: confirmed `adsbdb.com` (the API this
app used) sources flight-route data from the exact same VRS Standing Data
Maintenance source as adsb.lol itself — same crowd-sourced, callsign-keyed, no
date-range/versioning staleness problem, not a genuinely different or more
reliable source. Routes are keyed on callsign only (one static row per
callsign, e.g. `UAL123` → `DEN-JFK`), and flight numbers get reassigned to
different routes over time with no automatic correction — a human has to notice
and submit a fix to the SDM site. If this UI is ever brought back, the
underlying accuracy problem hasn't changed and needs to be weighed again.

**2026-08-06 update**: user says they've since found "a way to deal with"
this — flagged in the backlog as a real open item to revisit, no details on
the new approach given yet.

---

## 9. Map vs Radar visibility design (deliberate, not a bug)

Map draws and lets you tap aircraft beyond the bullseye range ring, all the way
to the rectangular canvas edges — intentional, explicitly confirmed by the user
after an earlier "fix" wrongly corrected it away. This is what differentiates
Map (uses the full screen, looks like a map) from Radar (clips strictly to the
circular bullseye ring, to look like a radar). `MapProjection::to_screen()`
only checks the rectangular canvas bound; `radar_view.cpp`'s
`to_radar_screen()` explicitly enforces a circular `dist_nm > radius_nm` cutoff.
**Do not add a radius cutoff to Map's draw/tap-hit-test, and do not loosen
Radar's circular clip.** Any "is this visible" question should ask the specific
active view, not assume one universal rule (see `map_view_aircraft_visible()`
as the established pattern for this).

---

## 10. OTA updates (application firmware, via GitHub Releases)

Built and iterated v0.1.0→v0.1.4 in one evening (2026-07-26), real-hardware-
tested end-to-end on both boards (back when CrowPanel still existed).
Deliberately does **not** touch the ESP32-C6 co-processor's own firmware (a
separate, harder problem — see §4/§5).

- `partitions.csv` redesigned for two OTA app slots (3MB each).
- `src/version.h` — dev builds always report `"v0.0.0-dev"` so a local build
  never accidentally reports "up to date."
- `src/data/ota.{h,cpp}` — state machine, piggybacked on the existing
  `location_poll_task` loop rather than a dedicated task (same DRAM-safety
  reasoning as §5).
- `.github/workflows/release.yml` — tag push builds and attaches firmware
  binaries to a GitHub Release.
- **v0.1.3** fixed a Settings layout overlap bug (a `LV_ALIGN_BOTTOM_MID`
  padding-math mistake) and addressed reported screen flashing during flash
  write by freezing the whole UI (`lv_timer_enable(false)`) during download.
- **v0.1.4**: user wanted progress feedback during the freeze — added a live
  0-100% bar, a **deliberate, bounded exception** to the flash-free freeze
  (needs its own explicit `lv_refr_now()` per tick). Message text updated to
  say flashing during this phase is expected/harmless. Explicitly not chasing
  a fully flash-free update — visibility judged more valuable once the *bulk*
  of the flashing (other views' redraw traffic) was already eliminated.
- Open: the fuller "Device column" Settings reorg (see backlog §7.1); the
  `.ps1` Windows OTA function was never syntax-checked (no pwsh in the dev
  environment).

---

## 11. Location-picker architecture

Moved from "APRT is a 5th swipeable tile" to a location-picker model: Home plus
up to 15 saved airports/waypoints (`src/data/locations.h/.cpp`), selectable via
a picker button. All views (Map/Radar/Arrivals/Stats) read from whichever
location is currently active. Runway diagrams draw inline in Map view instead
of a separate screen; the old `aprt_view.cpp`/`VIEW_APRT` was deleted.

`locations_add_from_icao()` fetches `airportdb.io`'s API and parses OurAirports
column names — all numeric fields arrive as JSON *strings*, not numbers, so
every numeric read uses `.as<float>()`/`.as<int>()`, not `| default`. Per-runway
`closed` field (also string-typed) must be checked — some airports (KORD) have
decommissioned runways that still carry valid coordinates.

A planned overhaul (see backlog §7.1's "Generalize Home" entry) would remove
the Home/saved-airport architectural split entirely — treat the "Home is
special-cased" description here as accurate for *current* code, not fixed
design, and double check current status before relying on it.

---

## 12. User preferences / how to work in this project (feedback memory)

- **Never add `Co-Authored-By: Claude...` to commits.** No exceptions. Past
  violations caused "claude" to appear as a GitHub contributor, which the user
  found unacceptable. Also: do not commit or push without explicit permission
  — make changes, then wait to be told to commit. Treat "no Co-Authored-By" as
  a hard checklist item on every single commit, not a one-time preference.
- **User handles firmware builds themselves.** Do not run `pio run` to verify
  changes — write the code and stop; note compile-error concerns in text if
  relevant, but don't build.
- **Justify, don't default**: when proposing to carry an existing tool/library/
  pattern forward into new work (e.g. porting to a new platform), state a real
  justification tied to the *new* context's actual constraints — don't just
  reuse it because it's already there. (This came from the LVGL-on-Pi decision
  — see §6.)

---

## 13. Cross-cutting lessons worth remembering

- **When "where exactly does X render" resists a few rounds of guessing, stop
  guessing from source and add a measurement ruler + ask for a real photo.**
  Used successfully twice (jc1060 bullseye centering, Pi bullseye centering).
- **Vendor docs and even a vendor's own GitHub issues can disagree with each
  other — trust real hardware/community reports over official examples**
  (CrowPanel SDIO pin values).
- **Any new FreeRTOS task on this ESP32-P4 board is a crash risk for network
  work, regardless of task lifetime** — piggyback on an existing task's loop.
- **`lv_obj_delete()` from inside an event handler on that object or a
  descendant is undefined behavior on this LVGL version** — use
  `lv_obj_delete_async()`.
- **Blocking NVS/flash writes over roughly a few hundred bytes, done
  synchronously on the UI/render thread, visibly stall this board's LCD panel**
  — keep such writes small or move them off the render path.
- **Once local instrumentation cleanly isolates a symptom's exact trigger,
  check *all* code paths that could react to that trigger — not just the one
  already under suspicion.** (The Pi tileview-wraparound bug was found this
  way after two other plausible-but-wrong theories were chased first.)
- **A shared cross-platform LVGL API can silently differ by resolved version**
  (`lv_draw_triangle_dsc_t`'s field names changed between LVGL 9.2.2 and
  9.5.0) — when only one platform build fails, check the *actual resolved*
  version on the working side; a `^9.2.2`-style semver range in
  `platformio.ini` does not tell you what's really installed.
