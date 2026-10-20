# Labplus mPython 3.0 (掌控板 3.0) — board id `mpython3_0`

This board port is **self-contained**: it consists only of the native board
manager YAML files plus the board-local C file in this directory. It does not
modify, add or delete anything under `components/`, so it stays compatible with
upstream esp-claw and survives rebases.

> The board id is `mpython3_0`, not `mpython3.0`. The board manager generator
> derives the Kconfig symbol as `ESP_BOARD_<NAME>` with `-` mapped to `_`
> (`gen_bmgr_config_codes.py`), and a `.` is not legal in a Kconfig symbol.

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3, dual-core LX7 @ 240 MHz |
| Flash | 16 MB quad NOR (GD25Q128ES1G), QIO 80 MHz |
| PSRAM | 8 MB quad SPI (ESP-PSRAM64), 80 MHz |
| Display | ST7789 1.47" 320x172 IPS over SPI |
| Audio codec | ES8388 (I2S + I2C) |
| Speaker amp | NS4150 class-D |
| Microphone | Dual onboard analog mics (through ES8388) |
| LED | 3x WS2812 (RGB) |
| Sensors | QMI8658C 6-axis, MMC5603NJ magnetometer, LTR-308ALS light |
| Buttons | A (GPIO0), B (GPIO46), 6 capacitive touch keys |
| Console | USB Serial/JTAG |

## GPIO Mapping

| Function | GPIO | Notes |
|----------|------|-------|
| LCD SCK | 36 | |
| LCD MOSI (SDA) | 37 | |
| LCD CS | 34 | |
| LCD DC (RS) | 35 | |
| LCD backlight | 33 | LEDC PWM |
| I2C SCL | 43 | Shared with onboard sensors and ES8388; pad P19 |
| I2C SDA | 44 | Shared with onboard sensors and ES8388; pad P20 |
| I2S MCLK | 39 | |
| I2S BCLK | 41 | |
| I2S WS (LRCK) | 42 | |
| I2S DOUT | 38 | ESP32-S3 -> ES8388 DIN |
| I2S DIN | 40 | ES8388 DOUT -> ESP32-S3 |
| WS2812 data | 8 | 3 LEDs, RMT |
| Button A | 0 | Pad P5, active low |
| Button B | 46 | Pad P11, active low |
| Touch keys P/Y/T/H/O/N | 9, 10, 11, 12, 13, 14 | Not modelled as a device |
| Buzzer | 21 | Pad P12, not modelled as a device |
| Sound sensor | 6 | Pad P10, analog |

## Build & Flash

Requires ESP-IDF v5.5.x and the ESP Board Manager helper package.

```bash
pip install esp-bmgr-assist

cd application/edge_agent

# 1. Confirm the board is discovered
idf.py bmgr -c ./boards -l

# 2. Select the board. ESP Board Manager picks the chip itself, so do NOT run
#    `idf.py set-target` (it would wipe sdkconfig and undo the board selection).
idf.py bmgr -c ./boards -b mpython3_0

# 3. Build
idf.py build

# 4. Erase first when the board still runs vendor firmware (MicroPython or the
#    xiaozhi image). Those ship a completely different partition layout, so
#    leftovers would collide with the ESP-Claw table.
idf.py -p <PORT> erase-flash

# 5. Flash and monitor
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with the USB Serial/JTAG port: `COMx` on Windows,
`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux.

### Download mode

The board has no USB-UART bridge; the console runs on the ESP32-S3 native USB
Serial/JTAG peripheral, which esptool can normally reset into download mode by
itself. If the port does not appear or flashing cannot sync, enter download mode
manually: button A is wired to the GPIO0 boot strapping pin, so

1. hold **A**,
2. press and release **Reset**,
3. release **A**.

Exit `idf.py monitor` with `Ctrl+]`.

## Pin Source Notes

Pin assignments come from the official 掌控板3.0 hardware documentation
([1.3 引脚排布及功能](https://mpython-esp32s3-doc.readthedocs.io/zh-cn/latest/1_hardware/1_3_IO.html))
and are cross-checked against the vendor MicroPython firmware
([labplus-cn/mpython_esp32s3](https://github.com/labplus-cn/mpython_esp32s3),
`port/boards/mpython_pro/`).

Two upstream inconsistencies are worth recording, because they affect this board
definition:

1. **PSRAM must be quad, not octal.** An octal PSRAM part on ESP32-S3 consumes
   GPIO33..37, which this board uses for the LCD and backlight. That would leave
   the SoC with no free pins for the panel at all, so the 8 MB PSRAM part is quad
   (`ESP-PSRAM64`), matching the vendor's per-board non-octal `sdkconfig.spiram`.
   `sdkconfig.defaults.board` therefore sets `CONFIG_SPIRAM_MODE_QUAD=y`.
2. **The vendor `mpconfigboard.h` I2C pins are stale.** It declares
   `MICROPY_HW_I2C0_SCL=35` / `SDA=34`, which collide with the documented LCD
   CS/DC pins (34/35), and the same file's `bsp_audio_board.h` is byte-identical
   to the sibling `labplus_Ledong_v2` board (its I2C block still carries a
   `labplus_classroom_kit` comment and its `bsp_i2c_init()` body is commented
   out). The documented external/internal I2C bus on GPIO43/44 is used here
   instead, which matches `labplus_Ledong_v2`.

The I2S pin group (MCLK=39, BCLK=41, WS=42, DOUT=38, DIN=40) is confirmed by both
sources and is not in doubt.

## Not Wired Up

The following onboard hardware is intentionally not declared as a board device
because esp-claw's board manager has no matching device type yet:

- The six capacitive touch keys (GPIO9..14).
- The piezo buzzer (GPIO21).

The QMI8658C / MMC5603NJ / LTR-308ALS sensors on the shared I2C bus are
deliberately **not** declared as board devices either. The bus itself is
declared, and the generic Lua `i2c` module reaches all of them at runtime, so no
board-manager device type and no component change is needed:

| Address | Part | Notes |
|---------|------|-------|
| `0x6B` | QMI8658C | `WHO_AM_I` (reg `0x00`) reads `0x05`; accel data starts at `0x35` |
| `0x30` | MMC5603NJ | `ProductID` (reg `0x39`) reads `0x10` |
| `0x53` | LTR-308ALS | ambient light |
| `0x10` | ES8388 | audio codec, owned by the `audio_dac` / `audio_adc` devices |
| `0x7E` | unidentified | responds to a scan, no public datasheet match |

```lua
local i2c = require("i2c")
local bus = i2c.new(0, 44, 43)          -- port 0, SDA, SCL
local dev = bus:device(0x6B)            -- QMI8658C
print(dev:read_byte(0x00))              -- WHO_AM_I -> 5
local raw = dev:read(12, 0x35)          -- length first, then reg: accel + gyro
```

`i2c.new()` reuses the bus the board manager already initialised on GPIO43/44
("I2C Bus V2 uses the externally initialized bus handle"), so the sensors and the
ES8388 share one bus without a second driver instance. Note the argument order:
`read(len[, reg])` and `write(data[, reg])` take the payload first, while
`read_byte(reg)` / `write_byte(value[, reg])` take the register first.

Verified on hardware against this board definition:

```
QMI8658 WHO_AM_I =	5
accel bytes	6
```

## Display Orientation

The glass is natively 172x320 portrait; `swap_xy: true` presents it as the
documented 320x172 landscape. The verified working combination is

```yaml
mirror_x: true
mirror_y: true
swap_xy: true
invert_color: true
```

which `setup_device.c` applies as `MADCTL = 0xE0` (MY|MX|MV). This was confirmed
on real hardware two independent ways: driving the panel through the board
manager and reading the resulting image, and a Lua three-bar test pattern that
showed the panel applies a 180-degree rotation when only MV is set.

`setup_device.c` also sets a 34-pixel vertical gap (`esp_lcd_panel_set_gap(0,
(240 - 172) / 2)`). Without it the panel shows a band of uninitialised pixels,
because the ST7789 frame memory is 240 rows tall while this glass only exposes
172 of them.

If the image ever comes up mirrored or rotated, flip `mirror_x` / `mirror_y` in
`board_devices.yaml` (and `MPYTHON3_0_LCD_MADCTL` in `setup_device.c` to match);
no other LCD setting should need to change.

## Runtime Configuration

There is no board-specific runtime configuration knob. Earlier drafts cycled the
LCD orientation at runtime from button B and a `/fatfs/lcd.cfg` file; that was
removed so the port stays purely declarative. Orientation is fixed at build time
by the YAML above, and anything else that needs to be adjustable (LEDC, ADC, I2C,
touch, RMT) is reachable through the stock Lua modules, which all accept GPIO
numbers at call time.

## Verified On Hardware

Built with ESP-IDF v5.5.2 on a Linux CI runner and flashed to a real 掌控板 3.0.
It boots with no errors; the lines that matter for this port:

```text
I (550) esp_psram: Found 8MB PSRAM device
I (550) esp_psram: Speed: 80MHz
I (563) esp_psram: Adding pool of 8161K of PSRAM memory to heap allocator
I (560) cpu_start: cpu freq: 240000000 Hz
I (612) BOARD_MANAGER: All peripherals initialized
I (613) MPYTHON3_0_SETUP_DEVICE: ST7789 ready: MADCTL=0xE0, y_gap=34 px
I (740) DEV_AUDIO_CODEC: Create esp_codec_dev success, dev:..., chip:es8388
I (743) DEV_BUTTON: Successfully initialized button: button_a, sub_type: gpio
I (744) DEV_BUTTON: Successfully initialized button: button_b, sub_type: gpio
I (821) display_service: started display service: 320x172
I (6661) cap_lua_rt: Lua runtime ready: registered_modules=25
I (6674) app_capabilities: Register session manager cap ok (groups=19, caps=73)
I (6696) claw_agent_mgr: Created root agent id=0
```

The only warnings are the expected "credentials not configured" notices for the
IM channels this board does not use.

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (id, chip, manufacturer) |
| `board_peripherals.yaml` | I2C, SPI, I2S, LEDC, RMT and GPIO pin configuration |
| `board_devices.yaml` | LCD, brightness, audio codec, LED strip and button devices |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
| `setup_device.c` | ST7789 panel factory entry point: MADCTL and the 34 px gap |

## Windows Build Note

`idf.py build` works unchanged on Linux and macOS. On Windows this board
currently fails while compiling the LCD stack: enabling `display_lcd` pulls in
lvgl, freetype, esp-dsp and eight panel drivers, which pushes one gcc command
line to roughly 33 kB and past the 32767-character `CreateProcess` limit
(`ninja: fatal: CreateProcess: The parameter is incorrect`). Use a Linux/macOS
host, WSL, or CI to build it.
