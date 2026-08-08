# ADS-B Radar Display

Real-time aircraft tracker on an ESP32-P4 with a 1024x600 touchscreen. Pulls live ADS-B data from [adsb.lol](https://api.adsb.lol) and displays aircraft across four views, centered on Home or any of up to 15 saved airports/locations.

![Views](https://img.shields.io/badge/views-Map%20%7C%20Radar%20%7C%20List%20%7C%20Stats-blue)
![Platform](https://img.shields.io/badge/platform-ESP32--P4-green)
![License](https://img.shields.io/badge/license-MIT-brightgreen)

> Also in progress: a parallel Raspberry Pi port (branch `pi-port`) sharing
> most of `src/data`/`src/ui` with this ESP32 build — see
> [`pi/README.md`](pi/README.md).

## Screenshots

*(placeholders — swap in current screenshots)*

| | |
|---|---|
| ![Main map screen](docs/images/map.JPG) | ![Aircraft detail](docs/images/detail.JPG) |
| Map | Aircraft detail card |
| ![Radar simulation](docs/images/radar.JPG) | ![List board](docs/images/arrivals.JPG) |
| Radar | List (split-flap board) |
| ![Stats](docs/images/stats.JPG) | ![Settings](docs/images/settings.PNG) |
| Stats | Settings |
| *(add)* | *(add)* |
| Location picker | VIEW menu |

## Locations: Home + saved airports

There's no single fixed center point. **Home** is one lat/lon/elevation set in Settings, and you can additionally save up to 15 airports by ICAO code (fetched from [airportdb.io](https://airportdb.io), including runway geometry). A picker chip in the status bar switches which one is active — every view (Map/Radar/List/Stats) redraws around whichever location is currently selected, with its own independent trail/tag/filter/range state remembered per view.

Each saved location (Home included) also has an optional **nearby-runways toggle** (eye icon in the picker row): when on, it fetches and caches full runway geometry for nearby *large* airports too, not just the active one — e.g. viewing KJFK can also show KLGA/KEWR's runways, not just glyphs. Medium/small airports and anything outside the toggle's radius still show as a plain "+ ICAO" glyph, sourced from a compiled-in static database of large/medium airports worldwide (`tools/generate_airports_db.py`).

## Views

- **Map** — Top-down projection with rotated aircraft icons (airliner/jet/GA/heli, color-coded by category), altitude-colored trails, runway diagrams for the active + nearby-toggled locations, and optional static pre-rendered OpenStreetMap backgrounds.
- **Radar** — Rotating sweep with phosphor-style fading blips, same runway/legend treatment as Map in a classic radar-scope look.
- **List** — Split-flap departure-board style table: callsign/registration, altitude, speed, vertical rate, status (GROUND/CRUISE/CLIMB/DESCEND), distance.
- **Stats** — System health (heap, PSRAM, FPS, RTOS tasks), network stats (IP, fetch/enrich ok/err counts, RSSI), and session tracking for the current location (unique aircraft, peak count, altitude/speed distributions, closest/fastest/highest records, top airlines/types) — ground traffic excluded from all of it.

All four views share:
- **Tap any aircraft** to open a scrollable detail card (operator, registration, type, altitude/speed/climb, squawk, photo credit).
- **Filters** (right-edge button column on Map/Radar/List): category filters COM / GA / HELI / MIL / EMG (any active one matches — OR), plus state filters VERT (ascending/descending), HIGH / LOW (altitude band, mutually exclusive) that further narrow whatever categories are active (AND), and a separate GND quick-toggle (mutually exclusive with VERT). Each view remembers its own filter selection independently.
- **VIEW menu** (Map/Radar only): trails on/off + amount, three independent tag fields (Flight ID, Alt/Speed, Type), and a secondary-locations visibility toggle — all per-view.
- **Adjustable range** — 4 user-configurable radius presets (Settings), cycled via a status-bar chip.
- **Auto-cycle** between views with a configurable interval, pausing on touch.
- **Alerts** — a toast for military aircraft and emergency squawks (7500/7600/7700); tap it to jump to that aircraft. Doesn't auto-switch views.

## Hardware

**Board:** JC1060P470C (ESP32-P4 RISC-V, 32MB PSRAM, 16MB flash)
- 1024x600 MIPI-DSI display (JD9165 controller)
- GT911 capacitive touchscreen
- Built-in 100Mbps Ethernet (IP101 PHY)
- ESP32-C6 WiFi co-processor, connected over SDIO via [ESP-Hosted](https://github.com/espressif/esp-hosted-mcu) — **see [Known Issues](#known-issues--limitations) before relying on WiFi**, there's real hardware/firmware fragility here worth understanding up front.

All display/touch/hardware drivers ship in this repo (`src/hal/`) — no vendor SDK download needed.

> This project was built for the JC1060P470C board. See [Adapting to Other Boards](#adapting-to-other-boards) for porting to different hardware.

---

## Getting Started

### Step 1: Install Software

You need **VS Code** and the **PlatformIO** extension.

<details>
<summary><strong>Windows</strong></summary>

1. Install [VS Code](https://code.visualstudio.com/), accept defaults.
2. In VS Code, open Extensions (`Ctrl+Shift+X`), search **"PlatformIO IDE"**, install, restart when prompted.
3. USB: the JC1060P470C uses USB CDC — no extra driver needed on Windows 10+. If your board isn't recognized, try the [CP210x](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) or [CH340](http://www.wch-ic.com/downloads/CH341SER_ZIP.html) driver.
4. Install [Git](https://git-scm.com/download/win), accept defaults.

</details>

<details>
<summary><strong>macOS</strong></summary>

1. Install [VS Code](https://code.visualstudio.com/), drag to Applications.
2. In VS Code, open Extensions (`Cmd+Shift+X`), search **"PlatformIO IDE"**, install, restart when prompted.
3. Git: run `git --version` in Terminal — macOS will offer to install Xcode Command Line Tools if it's missing.
4. USB: macOS includes CDC drivers already — no extra install needed for the JC1060P470C.

</details>

<details>
<summary><strong>Linux</strong></summary>

1. Install VS Code (Ubuntu/Debian: download the `.deb` and `sudo dpkg -i code_*.deb`; Arch: `sudo pacman -S code`; or `snap install --classic code`).
2. In VS Code, open Extensions (`Ctrl+Shift+X`), search **"PlatformIO IDE"**, install, restart when prompted.
3. USB permissions:
   ```bash
   sudo usermod -a -G dialout $USER
   ```
   Log out and back in, then verify with `groups`.
4. Install Git if needed (`sudo apt install git` / `sudo dnf install git` / `sudo pacman -S git`).

</details>

### Step 2: Get the Project

```bash
git clone https://github.com/dpoler/adsb.git
cd adsb
code .
```

PlatformIO will detect the project and download libraries/toolchains automatically — watch the bottom status bar on first open, this can take several minutes.

### Step 3: Build

In VS Code with PlatformIO: click the PlatformIO icon (alien head) in the sidebar → **PROJECT TASKS > jc1060 > Build**.

Or from the command line:

```bash
pio run -e jc1060       # JC1060P470C, 7" — the only supported target
```

See [Adapting to Other Boards](#adapting-to-other-boards) if you're bringing up something else entirely.

### Step 4: Flash

1. Connect the board via USB-C.
2. Find the serial port:

   | OS | How |
   |----|-----|
   | Windows | Device Manager → Ports (COM & LPT) |
   | macOS | `ls /dev/cu.usb*` |
   | Linux | `ls /dev/ttyACM* /dev/ttyUSB*` |

3. Flash — PlatformIO's **Upload** task, or:

   ```bash
   pio run -e jc1060 -t upload --upload-port /dev/ttyACM0   # adjust port
   ```

4. The board reboots automatically and the display comes up within a few seconds — **with no configuration at all**. WiFi/Ethernet, Home location, and every other setting are configured live from the on-screen Settings panel (gear icon, top-right of the status bar), not a build-time config file. There's nothing to edit and reflash for basic setup.

### Step 5: Configure on the device

Tap the gear icon:

- **Network** — WiFi SSID/password (or toggle to Ethernet, DHCP, no config needed), applies after a reboot.
- **Home** — latitude/longitude/elevation. [latlong.net](https://www.latlong.net/) is an easy way to find coordinates.
- **Radius presets** — your 4 zoom levels (default 5/10/20/50nm).
- **Alerts** — military / emergency squawk toasts on or off.
- Everything saves to NVS and survives reboots/reflashes.

**airportdb.io token** (only needed if you want to save airports with runway geometry, not just Home): get a free token at [airportdb.io](https://airportdb.io), then set it over USB serial rather than the on-screen keyboard — it's ~97 characters, impractical to type on a touchscreen:

```
TOKEN=your_airportdb_io_token_here
```

Send that line (115200 baud) via `pio device monitor` or any serial terminal, then hit Enter.

### Step 6 (optional): Static map backgrounds

Without this, Map view works fine on a plain dark background. To render real OpenStreetMap tiles behind it:

```bash
pip install requests Pillow
python tools/generate_static_map.py --lat YOUR_LAT --lon YOUR_LON
```

Rebuild and reflash afterward.

### Step 7 (optional): Static airport glyph database

Powers the "+ ICAO" glyphs for large/medium airports worldwide that you haven't explicitly saved (`draw_static_airport_glyphs()`). Without it, only your saved locations show airport markers.

```bash
python tools/generate_airports_db.py
```

Rebuild and reflash afterward.

### Troubleshooting

| Problem | Solution |
|---------|----------|
| Board not detected on USB | Try a different cable (some are charge-only) or port. On Linux, check `dmesg` after plugging in. |
| Display stays black after flash | Confirm `partitions.csv` is being used (the firmware image is too large for a default partition table). Try the board's reset button. |
| No aircraft appear | Check Settings — is WiFi/Ethernet actually connected? Open Stats to check network status/error counts. |
| WiFi seems stuck/slow on boot, or the device reboots on its own | See [Known Issues](#known-issues--limitations) below — this board has real, documented WiFi/co-processor fragility that isn't an app bug. |
| Upload fails with "connection timeout" | Hold the board's BOOT button while clicking Upload, release once upload starts. |

---

## Settings (on-device)

Gear icon in the status bar. Persists to NVS, survives reboots:

- WiFi SSID/password, or Ethernet toggle
- Home latitude/longitude/elevation
- 4 radius presets (5-500nm each)
- Metric units toggle
- Military / emergency alert toggles
- Auto-cycle on/off + interval
- airportdb.io token (via serial, see above)

Per-view settings (trails, tags, filters, secondary-locations visibility) live in each view's own **VIEW** menu and filter column instead, not here — see [Views](#views).

## Data Sources

| Source | Purpose | Auth |
|--------|---------|------|
| [api.adsb.lol](https://api.adsb.lol) | Live aircraft positions | None |
| [airportdb.io](https://airportdb.io) | Runway geometry + elevation for saved locations | Free token |
| [api.adsbdb.com](https://www.adsbdb.com) | Registration, operator, aircraft type enrichment | None |
| [planespotters.net](https://www.planespotters.net) | Photo credit text (no image rendering on ESP32 — see Known Issues; Pi downloads thumbnails) | None |
| [AeroDataBox](https://aerodatabox.com) (RapidAPI / API.Market / Direct) | Optional live flight origin/destination on the detail card (Pi) | Provider API key |

Static callsign→route tables (adsbdb routes, VRS standing data, etc.) are deliberately **not** used — they're frequently stale for reassigned flight numbers. Optional origin/destination on Pi comes from AeroDataBox's live flight-status API instead (off by default; set `adbox_key` + `adbox_prov` for RapidAPI / API.Market / Direct, then enable in Settings). Marketplace API-unit quotas reset on the **subscription billing cycle** (not a calendar month; see AeroDataBox FAQ) and remaining units are only on the provider dashboard — the app tracks a local **UTC calendar-month** call count (`adbox_n`) and can soft-cap (`adbox_lim`) or auto-disable on HTTP 429.

## Architecture

```
src/
  main.cpp              — Hardware init, LVGL setup, boot sequence
  pins_config.h          — GPIO pin definitions, display resolution
  hal/                   — Display (JD9165) and touch (GT911) drivers (included, no vendor SDK needed)
  data/
    aircraft.h           — Aircraft struct, AircraftList with FreeRTOS mutex
    fetcher.cpp           — Bulk ADS-B fetch, WiFi/Ethernet init + reconnect, C6 co-processor reset/recovery
    locations.h/.cpp      — Home + saved-airport locations, nearby-runways cache, airportdb.io fetch
    enrichment.cpp         — Per-aircraft adsbdb.com + planespotters.net enrichment (piggybacked on an existing task, see Known Issues)
    serial_config.cpp      — USB-serial TOKEN= command for the airportdb.io token
    http_mutex.h            — Global HTTP request serialization
    storage.h/.cpp          — NVS persistent settings (UserConfig)
  ui/
    views.cpp                — Tileview manager, auto-cycle timer, resume-on-boot
    map_view.cpp               — Map: projection, aircraft icons, trails, runways, legends
    radar_view.cpp              — Radar sweep, phosphor blips, runways, legends
    arrivals_view.cpp            — List: split-flap board
    stats_view.cpp                — System/network/session stats dashboard
    detail_card.cpp                — Aircraft detail overlay
    location_picker.cpp             — Home/saved-airport picker popover
    view_menu.cpp                    — Trails/tags/secondary-locations popover
    filters.cpp                       — Shared filter state (COM/GA/HELI/MIL/EMG/VERT/HIGH/LOW)
    display_prefs.cpp                  — Per-view trail/tag runtime accessors
    alerts.cpp                          — Military/emergency alert toast queue
    settings.cpp                         — On-device settings panel
    status_bar.cpp                        — Top bar: nav tabs, picker/range/VIEW chips, gear icon
    geo.h                                  — Shared lat/lon <-> screen projection math
tools/
  generate_static_map.py    — OSM tile fetcher for map backgrounds (optional)
  generate_airports_db.py    — Static large/medium airport glyph DB generator (optional)
```

## Known Issues & Limitations

### WiFi / ESP32-C6 co-processor — read this before filing a WiFi bug

This board's WiFi runs through an ESP32-C6 co-processor talking to the ESP32-P4 host over SDIO via [ESP-Hosted](https://github.com/espressif/esp-hosted-mcu). This link has real, hard-won fragility that isn't specific to this app's code:

- **Host and co-processor firmware versions must match**, or WiFi degrades (slow first-connect-after-boot) or breaks outright. The pioarduino platform version pinned in `platformio.ini` determines what ESP-Hosted protocol version the P4 host expects; the C6's own firmware is separate and must be kept in step manually via [`dpoler/c6_updater`](https://github.com/dpoler/c6_updater), a standalone tool that reflashes the C6 over the existing SDIO link (no extra wiring). Boot log always prints both versions (`ESP-Hosted versions: host vX.X.X, C6 co-processor vX.X.X`) — a mismatch shows up immediately there.
- **`55.03.37` / ESP-Hosted `2.11.6` is the only pairing confirmed stable on this board.** A newer, equally-matched pairing (`55.03.39` / `2.12.8`) was tried and reliably crashed with `assert failed: sdio_rx_get_buffer` within ~10 seconds of every single boot — worse than the older pairing, not better, despite being fully version-matched. **Don't bump the platform version (or run `c6_updater` targeting a newer C6 firmware) without a tested plan to revert** — matching versions alone does not guarantee stability, and "newer" has already been proven worse once on this exact board.
- **Even at the confirmed-good pairing, an intermittent `assert failed: sdio_rx_get_buffer` crash can still occur.** This is a confirmed **open upstream bug** in `espressif/esp-hosted-mcu` (issues [#144](https://github.com/espressif/esp-hosted-mcu/issues/144)/[#167](https://github.com/espressif/esp-hosted-mcu/issues/167)), root-caused by an Espressif engineer as DMA-capable memory fragmentation (plenty of total free heap, but no single contiguous block big enough for the SDIO driver's allocation) — not a bug in this app, and not currently fixable from application code or this project's build setup. Expect occasional unprompted reboots. If it becomes frequent, check the boot log's host/C6 version line first — a mismatch is the one contributing factor actually within reach to fix.
- **WiFi connect-after-boot behavior**: the app fast-fails a connect attempt the moment the driver reports a terminal failure (`WL_CONNECT_FAILED`/`WL_NO_SSID_AVAIL`) instead of waiting out a fixed timeout, and paces retries with a short delay rather than hammering `WiFi.begin()` back-to-back. If WiFi seems to take unusually long after a fresh boot or a `c6_updater` run, check the serial log's `[WiFi] status -> N at Nms` lines — they show exactly what the driver reported and when.

### Other known limitations

- **No aircraft photos rendered** — PSRAM-sourced images corrupt on this board's ESP32-P4 due to a cache-coherency issue. Photo *credit* text (from planespotters.net) is shown in the detail card instead of an image.
- **Screen may flash/glitch briefly during an OTA update** — the same ESP32-P4 cache-coherency issue behind the photo limitation above, showing up as visual tearing while `Update.write()` streams the new firmware to flash. Cosmetic only: the flash write itself isn't affected, and the device reboots normally into the new version once the download completes. Nothing to do here — let it finish.
- **Tile cache disabled** — `lv_draw_image` has rendering issues on this board's PPA. Static pre-rendered map backgrounds are used instead (see Step 6 above).
- **Small airports aren't in the static glyph database** — only `large_airport`/`medium_airport` (OurAirports classification, ~5,300 worldwide) are compiled in; adding ~42,700 more small airports was evaluated and deliberately declined (size/scan-cost vs. completeness). Save it explicitly by ICAO if you need one that's missing.
- **Screensaver — and brightness control along with it — is currently dormant.** Built, then disabled pending a redesign (the original burn-in rationale doesn't apply to LCD panels; brightness/dim/blank still have standalone value but need a different design, e.g. time-of-day scheduling, which isn't implemented). The brightness backend exists and works, but its only UI is the disabled screensaver popover — there is no working brightness control today.
- **USB CDC serial** can be unreliable on some units. Doesn't affect the display.

---

## Adapting to Other Boards

The architecture is portable to other ESP32 boards with displays. The easiest way to adapt it is with **[Claude Code](https://docs.anthropic.com/en/docs/claude-code)** — an AI coding assistant that can read the whole codebase and make targeted changes for your hardware.

### What You'll Need to Change

| Component | File(s) | What to change |
|-----------|---------|---------------|
| Pin definitions | `src/pins_config.h` | GPIO numbers for display, touch |
| Display driver | `src/hal/jd9165_lcd.cpp/.h`, `esp_lcd_jd9165.c/.h` | Replace with your display controller (e.g. ST7789, ILI9341, SSD1306) |
| Touch driver | `src/hal/gt911_touch.cpp/.h`, `esp_lcd_touch_gt911.c/.h` | Replace with your touch controller (e.g. FT5x06, CST816S, XPT2046) |
| Display resolution | `src/pins_config.h` | `LCD_H_RES` / `LCD_V_RES` |
| Display interface | `src/main.cpp` | MIPI-DSI → SPI/I2C/RGB parallel depending on your panel |
| Network | `src/data/fetcher.cpp` | Ethernet PHY config, or WiFi-only if no Ethernet/no hosted co-processor |
| UI layout | `src/ui/*.cpp` | Adjust for a different resolution |
| PlatformIO config | `platformio.ini` | Board type, framework, partition table |
| Partition table | `partitions.csv` | Adjust app partition size for your flash capacity |

### Using Claude Code to Port

1. Install Claude Code per [the docs](https://docs.anthropic.com/en/docs/claude-code).
2. `cd adsb && claude`
3. Describe your hardware, e.g.:
   > "I have a LilyGo T-Display-S3 with a 170x320 ST7789 SPI display and no touch. Adapt this project for my board."
4. Review the changes, build, flash.

### Porting Tips

- **Start with the display driver.** A colored rectangle on screen means the rest follows.
- **Smaller displays** (320x240, 480x320) need real UI layout rework — the List board and Stats dashboard are laid out for 1024x600 and won't fit as-is.
- **ESP32-S3 boards** are the most common alternative but have less RAM than the P4 — reduce render buffer sizes and `MAX_AIRCRAFT` if needed.
- **No-touch boards** can drop the touch driver/input device; auto-cycle rotates through views on its own.
- **If your board doesn't use an ESP-Hosted SDIO co-processor for WiFi** (i.e. it has native WiFi, unlike this board's P4+C6 split), the entire [Known Issues](#known-issues--limitations) WiFi section doesn't apply to you — that fragility is specific to this exact hardware split, not this app's WiFi handling in general.

## License

[MIT](LICENSE)
