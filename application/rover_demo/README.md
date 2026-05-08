# rover_demo

PlatformIO is the preferred build entry point for this application.

## Setup

```bash
cd application/rover_demo
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
```

## Build

```bash
cd application/rover_demo
PLATFORMIO_CORE_DIR=.pio-core .venv/bin/pio run
```

## Flash And Monitor

```bash
cd application/rover_demo
PLATFORMIO_CORE_DIR=.pio-core .venv/bin/pio run -t upload
PLATFORMIO_CORE_DIR=.pio-core .venv/bin/pio device monitor
```

The `m5stickc_plus` environment uses PlatformIO's `m5stick-c` ESP32 board
definition, ESP-IDF framework mode, `partitions.csv`, and the local
`sdkconfig.defaults`. ESP-IDF component dependencies are declared in
`main/idf_component.yml`, including the repo-local `cap_rover`, `cap_unitv`,
and ESP-Claw core components.

The pre-build script `scripts/pio_fatfs.py` generates `.pio/build/.../storage.bin`
from `fatfs_image/` so the firmware and seeded FATFS content are flashed together.

`PLATFORMIO_CORE_DIR=.pio-core` keeps PlatformIO packages and locks inside this
project directory instead of `~/.platformio`, which makes the build work in
sandboxed or read-only-home environments.
