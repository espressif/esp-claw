# RockBase-iot NM-Display-28inch

ESP-Claw board package for the [RockBase-iot NM-Display-28inch](https://github.com/RockBase-iot/NM-Display-28inch), a 2.8 inch ESP32-S3 smart display board jointly created by RockBase-iot and the NMTech team.

This package follows the same YAML-driven `esp_board_manager` pattern used by the other `application/edge_agent/boards` entries. The hardware inventory lives in `board_info.yaml`, `board_peripherals.yaml`, and `board_devices.yaml`. `setup_device.c` provides two factory hooks: `io_expander_factory_entry_t` initializes the AXP2101 PMU (enabling all DC-DC and LDO power rails) and drives the TCA9554 IO-expander to hardware-reset the LCD/Touch module; `lcd_panel_factory_entry_t` creates the ST7789 panel driver.

## Hardware Overview

| Feature | Specification |
| --- | --- |
| SoC | ESP32-S3 dual-core Xtensa LX7 @ 240 MHz |
| Flash / PSRAM | 16 MB QIO flash, 8 MB OPI PSRAM |
| Display | 2.8 inch ST7789 IPS LCD, 320x240 logical resolution |
| Touch | FT6336 capacitive touch over I2C |
| PMU | AXP2101 on shared I2C bus |
| Audio | ES8311 codec over I2S + I2C control |
| IMU | QMI8658 on shared I2C bus |
| RTC | PCF85063 on shared I2C bus |
| IO expander | TCA9554 on shared I2C bus; initialized at boot and resets LCD/Touch module via PIN_1 |
| Camera | DVP camera interface, RGB565 320x480 in the reference firmware |
| Storage | microSD slot in SDMMC 1-bit mode |
| Button | BOOT button on GPIO0, active low |

## GPIO Mapping

| Function | GPIO |
| --- | --- |
| I2C SCL | GPIO7 |
| I2C SDA | GPIO8 |
| LCD MOSI | GPIO1 |
| LCD SCLK | GPIO5 |
| LCD DC | GPIO3 |
| LCD CS | NC (`-1`) |
| LCD reset | NC (`-1`) |
| Backlight PWM | GPIO6 |
| I2S MCLK | GPIO12 |
| I2S BCLK | GPIO13 |
| I2S LRCK / WS | GPIO15 |
| I2S DIN / record | GPIO14 |
| I2S DOUT / playback | GPIO16 |
| SDMMC D0 | GPIO9 |
| SDMMC CMD | GPIO10 |
| SDMMC CLK | GPIO11 |
| Camera XCLK | GPIO38 |
| Camera VSYNC | GPIO17 |
| Camera HREF | GPIO18 |
| Camera PCLK | GPIO41 |
| Camera D0-D7 | GPIO45, GPIO47, GPIO48, GPIO46, GPIO42, GPIO40, GPIO39, GPIO21 |

## ESP-Claw Device Coverage

Defined in `board_devices.yaml`:

- `gpio_expander`: TCA9554 IO-expander at I2C address `0x20`. **Initialized first**. `io_expander_factory_entry_t` in `setup_device.c` enables all AXP2101 power rails via masked I2C register updates, then drives PIN_1 low -> 100 ms -> high to hardware-reset the LCD/Touch module before any other device starts.
- `display_lcd`: ST7789 over SPI2, 320x240, SPI mode 3, 80 MHz pixel clock, **RGB color order, little-endian data (swapbytes)**, inverted colors, landscape rotation (`swap_xy: true`, `mirror_y: true`).
- `lcd_brightness`: LEDC backlight control on GPIO6, active high, 5 kHz.
- `lcd_touch`: metadata-only FT6336 I2C capacitive touch reservation at `0x38`, 320x240 coordinates. It is `init_skip: true` because the current ESP-IDF component set does not include an FT6336 controller driver.
- `audio_dac`: ES8311 playback path, I2S output plus I2C control address `0x30`.
- `audio_adc`: ES7210-compatible capture metadata on I2S input plus I2C address `0x80`.
- `fs_sdcard`: SDMMC 1-bit microSD slot using D0/CMD/CLK on GPIO9/10/11.

Hardware documented but not yet initialized by this package: QMI8658 IMU, PCF85063 RTC, and DVP camera. Their occupied GPIOs are listed above for future reference.

## Build And Flash

```powershell
cd application/edge_agent
idf.py set-target esp32s3
idf.py bmgr --customer-path ./boards -b nm_display_28inch
idf.py build
idf.py -p <PORT> flash monitor
```

## Validation Checklist

Use this list when preparing or reviewing a PR for this board:

- `idf.py bmgr --customer-path ./boards -b nm_display_28inch` generates board-manager code without YAML errors.
- `idf.py build` completes for target `esp32s3`.
- Device boots into the ESP-Claw agent loop.
- Captive portal loads and can save Wi-Fi / LLM / IM settings.
- `display_lcd` initializes and renders the ESP-Claw mascot or a Lua display demo.
- Backlight responds through `lcd_brightness` / display brightness controls.
- FT6336 touch is reserved in board metadata; enable and smoke-test it after adding a compatible controller driver.
- Lua can call `board_manager.get_display_lcd_params("display_lcd")` and `display.init(...)`.
- SD card mount is tested if a card is inserted.

## PR Summary Template

Add support for the RockBase-iot NM-Display-28inch board.

Description

This board package follows the existing `esp_board_manager` YAML-driven pattern used by other supported ESP-Claw boards.

The following hardware definitions were added:

Chip:

ESP32-S3, 16 MB QIO NOR flash, 8 MB OPI PSRAM, dual-core @ 240 MHz.

Display:

2.8 inch ST7789 IPS LCD, 320x240 logical resolution. Connected via SPI2, write-only: MOSI=GPIO1, CLK=GPIO5, CS not connected, DC=GPIO3, reset not connected. SPI mode 3, 80 MHz clock, RGB color order, little-endian (swapbytes) data, inverted colors, 270-degree landscape rotation. The LCD and touch module are hardware-reset via TCA9554 PIN_1 on every boot.

Touch:

FT6336 I2C capacitive touch on the shared I2C bus, SDA=GPIO8, SCL=GPIO7, address 0x38. The entry is metadata-only in this PR because no matching FT6336 component is available in the current ESP-IDF component set.

Backlight:

GPIO6 active-high PWM via LEDC @ 5 kHz.

Storage:

microSD card slot in SDMMC 1-bit mode: D0=GPIO9, CMD=GPIO10, CLK=GPIO11.

Audio:

ES8311-compatible I2S/I2C playback path with MCLK=GPIO12, BCLK=GPIO13, LRCK=GPIO15, DIN=GPIO14, DOUT=GPIO16.

Related

https://github.com/RockBase-iot/NM-Display-28inch

Testing

I validated that board-manager code generation and the ESP-Claw firmware build succeed. Hardware smoke testing should cover display rendering, backlight, settings portal, Lua display APIs, SD card mounting, and FT6336 touch after a compatible driver is enabled.
