# Current Board Hardware: esp32_S3_DevKitC_1_breadboard

Read this skill before operating hardware, assigning GPIOs, or writing Lua and board-specific code.

## Rules
- Before operating any hardware, read this skill first.
- Before assigning a GPIO, check whether it is already occupied below.
- When writing Lua or board-specific code, use the listed device names instead of guessing hardware wiring.

## Board Summary
- Board: `esp32_S3_DevKitC_1_breadboard`
- Chip: `esp32s3`
- Version: `1.0.0`
- Manufacturer: `ESPRESSIF`
- Description: ESP32-S3-DevKitC-1 Development Board

## Device Inventory

### led_strip
- Occupied IO:
  - `channel_config.tx.gpio` -> `GPIO38`

### camera
- Occupied IO: none declared

### audio_dac
- Occupied IO: none declared

### audio_adc
- Occupied IO: none declared

### display_lcd
- Occupied IO:
  - `spi.panel_config.reset` -> `GPIO6`
  - `spi.cs` -> `GPIO15`
  - `spi.dc` -> `GPIO7`
  - `mosi` -> `GPIO5`
  - `sclk` -> `GPIO4`

## Notes
- If a device has no explicit IO mapping here, treat it as unknown instead of guessing.
