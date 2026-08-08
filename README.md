# FlightLevel314

Raspberry Pi ADS-B aircraft display — Map, Radar, Arrivals, Stats — on a
Waveshare 10.1" DSI touchscreen (1280×800). Live traffic from adsb.lol,
saved locations, basemap/weather overlays, and optional AeroDataBox
origin/destination + AirportDB.io runway enrichment.

**Name:** Flight Level 314… because π. Yes.

## Lineage

Forked from [dpoler/adsb](https://github.com/dpoler/adsb) (Pi port of the
JC1060 / ESP32-P4 display). The ESP32 target is **paused** — this repo is
Pi/Linux only. Features can be cherry-picked back to `dpoler/adsb` later if
desired. Shared `src/` still has some `#if defined(ARDUINO)` leftovers from
the dual-target era; harmless on Linux, cleanup is backlog.

## Status

Map / Radar / Arrivals / Stats render live traffic in a swipeable tileview
with status bar, VIEW menu, alerts, saved locations (ICAO or lat/lon), and
Settings. DRM/KMS + libinput kiosk mode verified on Pi hardware. Detail-card
enrichment (adsbdb + planespotters photos, optional AeroDataBox O/D) is live.

API keys are never typed on-device — hand-edit
`~/.config/flightlevel314/config.json` (`apt_tok`, `adbox_key`), pick the
AeroDataBox gateway in Settings, then enable services under API KEYS.

Migrating from the old `adsb` Pi port:

```bash
mkdir -p ~/.config/flightlevel314
cp -a ~/.config/adsb/. ~/.config/flightlevel314/
# optional caches:
cp -a ~/.config/adsb/basemap ~/.config/flightlevel314/ 2>/dev/null || true
cp -a ~/.config/adsb/weather ~/.config/flightlevel314/ 2>/dev/null || true
```

## Hardware

Waveshare 10.1" DSI capacitive touch (1280×800) on a Raspberry Pi, Raspberry
Pi OS Lite (no desktop) — the app owns DRM/KMS as a systemd kiosk service.

> Waveshare lists Pi 5/4B/**3B+**/3A+/CM3/3+/4 — verify before assuming a
> plain 3B works.

## Build (macOS / Linux SDL simulator)

```bash
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
cmake --build build -j$(nproc)
./build/pi/flightlevel314
```

## Build (Pi DRM)

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev \
    libdrm-dev libinput-dev pkg-config
# SDL not required for DRM-only builds
cmake -S . -B build -DPI_DISPLAY_BACKEND=DRM
cmake --build build -j4
./build/pi/flightlevel314
```

Add your user to `video`, `render`, and `input` if needed.

## Kiosk install

```bash
sudo useradd -r -G video,input,render flightlevel314
sudo mkdir -p /opt/flightlevel314
sudo cp build/pi/flightlevel314 /opt/flightlevel314/

sudo mkdir -p /opt/flightlevel314/.config/flightlevel314
sudo cp ~/.config/flightlevel314/*.json /opt/flightlevel314/.config/flightlevel314/ 2>/dev/null || true
sudo chown -R flightlevel314:flightlevel314 /opt/flightlevel314/.config

sudo cp pi/flightlevel314.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now flightlevel314
```

Logs: `journalctl -u flightlevel314 -f`.

## Layout

| Path | Role |
|------|------|
| `pi/` | Linux entrypoint, display/input, basemap/weather, platform_linux |
| `src/ui`, `src/data` | Shared UI + data (historical ARDUINO forks remain) |
| `tools/` | Airport DB / static map generators |
| `docs/project-knowledge.md` | History + backlog |

## Publishing this fork to GitHub

Canonical repo: **https://github.com/dpoler/FlightLevel314**

Local clone: `~/Projects/FlightLevel314`. The paused ESP32 line stays in
`dpoler/adsb` / `~/Projects/adsb`.
