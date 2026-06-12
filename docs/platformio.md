# PlatformIO Build Guide

## Requirements

- Python 3.8+
- PlatformIO Core 6.1+ (`pip install platformio==6.1.19`)
- `construct` library: `pip install construct==2.10.70`

Or use the project's virtualenv:

```bash
cd application/wave_rover
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Build

```bash
cd application/wave_rover
.venv/bin/pio run
```

## Flash

```bash
.venv/bin/pio run -t upload
```

## Monitor

```bash
.venv/bin/pio device monitor
```

## Environment

| Key | Value |
|---|---|
| Platform | espressif32 7.0+ |
| Board | esp32dev |
| Framework | espidf (IDF 6.x) |
| Flash mode | dio |
| Monitor speed | 115200 |
| Upload speed | 921600 |

## Notes

- First build downloads managed components via the IDF component manager.
- `sdkconfig.defaults` sets Wi-Fi, HTTP server, and CPU tuning. Do not edit `sdkconfig.wave_rover` directly.
- `dry_run = true` by default — no hardware is accessed until you change the NVS config.
- The `components/json/` local wrapper provides IDF 5.x `json` compatibility under IDF 6.x.
