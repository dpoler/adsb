# FlightLevel314 — `pi/` tree

Linux/Raspberry Pi application code (display, input, basemap, weather,
`platform_linux/`). Build from the **repo root** — see the top-level
[README.md](../README.md).

```bash
cmake -S .. -B ../build -DPI_DISPLAY_BACKEND=SDL   # from here, or from root:
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
cmake --build build -j$(nproc)
./build/pi/flightlevel314
```

Binary: `flightlevel314`. Config: `~/.config/flightlevel314/`.
Systemd unit: `pi/flightlevel314.service`.
