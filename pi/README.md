# ADS-B Radar Display — Raspberry Pi port

A parallel build target for the same app as the repo root, targeting a
Raspberry Pi + DSI touchscreen instead of the ESP32-P4 (jc1060) board.
Shares most of `src/data/` and `src/ui/` with the ESP32 build — see each
file's own comments for the `#if defined(ARDUINO)` forks where platform
behavior genuinely differs, and `src/platform/platform.h` for the
mutex/time/HTTP/config-storage seam that makes the sharing possible.

**Status**: Map/Radar/Arrivals/Stats all render live adsb.lol traffic in a
real swipeable tileview, with a real status bar (nav tabs/gear/range
chip), VIEW chip popover (trails, tag fields, secondary-location
visibility), alerts (military/emergency toasts), a real saved-locations
system (add/remove/reorder, ICAO or plain lat/lon), and a scoped-down
Settings screen. Real DSI hardware bring-up (DRM/KMS + libinput) is
confirmed working against physical hardware (Pi 3B+, Waveshare 10.1"
panel) — touch, swipe, and the full network fetch/render loop all
verified. Not yet ported: metar. Airline names load from
dpoler/AirlinesCSV at app start; detail-card enrichment (adsbdb +
planespotters photos, optional AeroDataBox origin/destination) is live
on Pi. API keys are never typed on-device — hand-edit
`~/.config/adsb/config.json` (`apt_tok` for airportdb.io,
`adbox_key` for AeroDataBox), pick the AeroDataBox gateway in Settings
(RapidAPI / API.Market / Direct), then use Settings → API KEYS to
see whether each key is present/valid and to enable or disable the
service (`apt_en` / `adbox_en`, plus `adbox_prov` 0/1/2).

## Hardware target

Waveshare 10.1" DSI capacitive touch panel (1280×800) on a Raspberry Pi
3B, running Raspberry Pi OS Lite (no desktop) — the app runs directly
over DRM/KMS as a systemd kiosk service, not inside a window manager.

> ⚠️ Waveshare's own listing for this panel names compatibility with Pi
> 5/4B/3B+/3A+/CM3/3+/4 — that's **3B+**, not the plain 3B. Verify with
> Waveshare (or the product page's fine print) before assuming this
> combination works.

## Building on macOS (dev loop)

No Pi or VM needed for UI/data work — LVGL's SDL2 simulator runs natively.

```
brew install cmake sdl2   # if not already installed
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
cmake --build build -j8
./build/pi/adsb_pi
```

Config persists to `~/.config/adsb/config.json` (or
`$XDG_CONFIG_HOME/adsb/config.json`). Delete it to reset to defaults.

## Building on the Pi (real hardware)

Build-tested against real hardware (Pi 3B+, Debian 13/trixie,
`6.18.34+rpt-rpi-v8`, Waveshare 10.1" DSI panel). The panel needs
exclusive DRM access, so a running desktop's display manager needs to be
out of the way first — either install Raspberry Pi OS Lite from the
start, or free up an existing desktop install without reinstalling:
`sudo systemctl set-default multi-user.target && sudo systemctl disable
--now lightdm` (swap `lightdm` for whatever your image actually runs;
`systemctl list-units --type=service --state=running | grep -i display`
will show it).

```
sudo apt install build-essential cmake libsdl2-dev libcurl4-openssl-dev \
    libdrm-dev libinput-dev pkg-config
cmake -S . -B build -DPI_DISPLAY_BACKEND=DRM   # ~3min, first run only
cmake --build build -j4
./build/pi/adsb_pi   # no sudo needed if your user is already in the
                      # video/render/input groups (`groups` to check);
                      # otherwise: sudo usermod -aG video,render,input $USER
```

`display_drm.cpp` assumes the panel enumerates as `/dev/dri/card0`;
`input_libinput.cpp` auto-locates the touch device by capability. Both
assumptions held on the first real run — no changes needed.

## Installing as a kiosk service

```
sudo useradd -r -G video,input,render adsb
sudo mkdir -p /opt/adsb-pi
sudo cp build/pi/adsb_pi /opt/adsb-pi/

# The service pins HOME=/opt/adsb-pi (see adsb-pi.service) since the
# `adsb` user has no real home directory -- seed its config there.
# storage_linux.cpp/locations_linux.cpp only mkdir() one level deep, so
# .config/adsb must already exist before the service's first launch.
sudo mkdir -p /opt/adsb-pi/.config/adsb
# Reuse whatever you already set up testing manually (saved locations,
# airportdb.io token), if any:
sudo cp ~/.config/adsb/*.json /opt/adsb-pi/.config/adsb/ 2>/dev/null || true
sudo chown -R adsb:adsb /opt/adsb-pi/.config

sudo cp pi/adsb-pi.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now adsb-pi
```

Logs: `journalctl -u adsb-pi -f`.

## Architecture notes

- Everything Pi/Linux-only lives under `pi/`, outside `src/` — PlatformIO's
  default `src_dir` is `src/` and auto-compiles everything under it, so
  Linux-only headers (libcurl, `<mutex>`, DRM) can't live there without
  breaking the jc1060 build.
- LVGL version is pinned in `pi/CMakeLists.txt` to whatever
  `.pio/libdeps/jc1060/lvgl/library.json` actually resolves for the ESP32
  build (currently 9.5.0) — platformio.ini's `^9.2.2` is a floating range,
  not the real installed version, and shared code (e.g.
  `src/ui/aircraft_icons.h`) uses APIs that differ across LVGL 9.x point
  releases. If a fresh jc1060 build resolves a newer version and something
  here stops compiling, re-check that before assuming it's this port's bug.
- `pi/app_stubs.cpp` holds temporary link-satisfying implementations for
  everything not yet ported (currently just metar) —
  each gets deleted as the real thing lands.
- `src/data/fetcher.cpp` (jc1060's WiFi/C6-co-processor fetch loop) is
  deliberately untouched and not shared — see its own extensive comments
  and `project_p4_heap_constraints`/`project_platform_pin` history for why
  that code is not worth risking a refactor on. `pi/platform_linux/
  datasource_remote.cpp` reimplements the adsb.lol JSON parsing fresh
  instead.
