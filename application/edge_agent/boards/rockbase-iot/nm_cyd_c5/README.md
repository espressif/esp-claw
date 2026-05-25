# RockBase-iot NM-CYD-C5

ESP-Claw board package for the [RockBase-iot NM-CYD-C5](https://github.com/RockBase-iot/NM-CYD-C5), an ESP32-C5 Cheap Yellow Display style board with dual-band Wi-Fi 6, BLE 5, Thread/Zigbee radio support, 16 MB flash, 8 MB PSRAM, and a 2.8 inch LCD.

## Hardware Overview

| Feature | Specification |
| --- | --- |
| SoC | ESP32-C5-WROOM-1, RISC-V @ 240 MHz |
| Flash / PSRAM | 16 MB flash, 8 MB quad PSRAM |
| Display | 2.8 inch ST7789 TFT LCD, 320x240 logical landscape resolution |
| Touch | XPT2046 resistive touch controller on the shared SPI2 bus |
| Storage | microSD slot on shared SPI2 bus |
| RGB LED | Single WS2812 pixel on GPIO27 |
| GPS connector | LP-UART P5, intended for NM-ATGM336H style NMEA GPS modules |
| Expansion | CN1 I2C header, P1 GPIO header, 12-pin FPC2 extension |
| Button | BOOT button on GPIO0, active low |

## GPIO Mapping

| Function | GPIO |
| --- | --- |
| SPI SCLK | GPIO6 |
| SPI MISO | GPIO2 |
| SPI MOSI | GPIO7 |
| LCD CS | GPIO23 |
| LCD DC | GPIO24 |
| LCD reset | Board reset (`-1`) |
| Backlight PWM | GPIO25 |
| Touch CS | GPIO1 |
| microSD CS | GPIO10 |
| WS2812 DIN | GPIO27 |
| GPS RX | GPIO4 |
| GPS TX | GPIO5 |
| I2C SDA | GPIO9 |
| I2C SCL | GPIO8 |
| BOOT | GPIO0 |

## ESP-Claw Device Coverage

Defined in `board_devices.yaml`:

- `display_lcd`: ST7789 over SPI2, 320x240, 40 MHz, landscape orientation (`swap_xy: true`, `mirror_y: true`).
- `lcd_brightness`: LEDC backlight control on GPIO25, active high, 5 kHz.
- `led_strip`: metadata-only WS2812 entry on GPIO27. Runtime scripts create and release the LED strip driver on demand to avoid RMT channel leaks.
- `fs_sdcard`: metadata-only SPI microSD entry on the shared SPI bus, CS=GPIO10. It is `init_skip: true` so mounting is explicit and does not block LCD use when no card is inserted.
- `lcd_touch`: metadata-only XPT2046 entry, CS=GPIO1. The shared SPI wiring is reserved for future or custom touch drivers.
- `gps_uart`: metadata-only NMEA UART entry, RX=GPIO4, TX=GPIO5, 9600 baud.
- `buttons`: metadata-only BOOT button entry on GPIO0.

The board also ships Lua helpers in the ESP-Claw FATFS image for screen display, backlight, RGB LED, and GPS usage. XPT2046 touch is reserved in metadata; scripts that need input should provide a BOOT-button fallback unless SPI touch support is enabled.

## Build And Flash

```powershell
cd application/edge_agent
idf.py set-target esp32c5
idf.py bmgr --customer-path ./boards -b nm_cyd_c5
idf.py build
idf.py -p <PORT> flash monitor
```

## Validation Checklist

Use this list when preparing or reviewing a PR for this board:

- `idf.py bmgr --customer-path ./boards -b nm_cyd_c5` generates board-manager code without YAML errors.
- `idf.py build` completes for target `esp32c5`.
- Device boots into the ESP-Claw agent loop.
- Captive portal loads and can save Wi-Fi / LLM / IM settings.
- `display_lcd` initializes and renders the ESP-Claw mascot or a Lua display demo.
- Backlight responds through `display.brightness(...)` or the `nm_cyd_c5_backlight` Lua script.
- `nm_cyd_c5_screen` can show color/text/message/clear modes on the LCD.
- `nm_cyd_c5_rgb` can set solid colors, blink, and run a short rainbow on GPIO27.
- `nm_cyd_c5_gps` can read NMEA sentences from a GPS module on P5 if connected.
- SD card mount is tested manually if a card is inserted.

## PR Summary Template

Add support for the RockBase-iot NM-CYD-C5 board.

Description

This board package follows the existing `esp_board_manager` YAML-driven pattern used by other supported ESP-Claw boards and ports the previously validated NM-CYD-C5 basic_demo configuration into `application/edge_agent/boards`.

The following hardware definitions were added:

Chip:

ESP32-C5-WROOM-1, 16 MB flash, 8 MB quad PSRAM, RISC-V @ 240 MHz.

Display:

2.8 inch ST7789 TFT LCD, 320x240 logical landscape resolution. Shared SPI2 bus: MOSI=GPIO7, MISO=GPIO2, CLK=GPIO6, LCD CS=GPIO23, DC=GPIO24, reset tied to board reset. Backlight is GPIO25 active-high PWM via LEDC @ 5 kHz.

Storage:

microSD card slot on the shared SPI2 bus, CS=GPIO10. The device is metadata-only and mounted on demand to avoid bus contention.

Touch:

XPT2046 resistive touch on the shared SPI2 bus, CS=GPIO1. The device is reserved as metadata for future or custom SPI touch support.

Expansion:

WS2812 RGB LED on GPIO27, GPS UART on GPIO4/GPIO5, I2C header on GPIO9/GPIO8, BOOT button on GPIO0.

Related

https://github.com/RockBase-iot/NM-CYD-C5

Testing

I validated that board-manager code generation and the ESP-Claw firmware build succeed. Hardware smoke testing should cover display rendering, backlight, RGB LED, GPS UART, settings portal, Lua display APIs, and optional SD card mounting.