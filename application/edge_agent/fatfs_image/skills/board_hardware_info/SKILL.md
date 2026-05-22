---
{
  "name": "board_hardware_info",
  "description": "Use this skill before operating hardware or writing Lua and board-specific code that depends on device inventory and occupied GPIOs.",
  "metadata": {
    "cap_groups": ["cap_boards"],
    "manage_mode": "readonly"
  }
}
---

# Current Board Hardware: nm_cyd_c5

Read this skill before operating hardware, assigning GPIOs, or writing Lua and board-specific code. **You cannot speculate or fabricate hardware information.**

## Rules
- Before operating any hardware, read this skill first.
- Before assigning a GPIO, check whether it is already occupied below.
- When writing Lua or board-specific code, use the listed device names instead of guessing hardware wiring.

## Board Summary
- Board: `nm_cyd_c5`
- Chip: `esp32c5`
- Version: `1`
- Manufacturer: `unknown`

## Device Inventory

The following devices are known to be present on this board:

### display_lcd
- Occupied IO:
  - `cs` -> `GPIO23`
  - `dc` -> `GPIO24`
  - `mosi` -> `GPIO7`
  - `miso` -> `GPIO2`
  - `sclk` -> `GPIO6`

### led_strip
- Occupied IO: none declared

### fs_sdcard
- Occupied IO:
  - `cs` -> `GPIO10`
  - `mosi` -> `GPIO7`
  - `miso` -> `GPIO2`
  - `sclk` -> `GPIO6`

### lcd_touch
- Occupied IO:
  - `mosi` -> `GPIO7`
  - `miso` -> `GPIO2`
  - `sclk` -> `GPIO6`

## Notes
- If a device has no explicit IO mapping here, treat it as unknown instead of guessing.
