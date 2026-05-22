# Lua SSD1306

This module provides a reusable pure-Lua SSD1306 driver built on top of the
existing `i2c` module. It is intended for I2C-connected SSD1306 OLED panels,
with `128x64 @ 0x3C` as the default configuration.

## How to call
- Load the driver with `local ssd1306 = dofile("/fatfs/scripts/ssd1306.lua")`
  - If your build only synced builtin scripts, `dofile("/fatfs/scripts/builtin/ssd1306.lua")`
    also works.
- Create an I2C bus and device first:
  - `local i2c = require("i2c")`
  - `local bus = i2c.new(port, sda, scl [, freq_hz])`
  - `local dev = bus:device(addr [, clk_speed])`
- Create the display object:
  - `local oled = ssd1306.new(dev [, opts])`

## Options
- `width`: default `128`
- `height`: default `64`
- `addr`: optional metadata only, default `0x3C`
- `external_vcc`: default `false`
- `segment_remap`: default `true`
- `com_scan_dec`: default `true`

Supported panel sizes are `128x64` and `128x32`.

## Methods
- `oled:init()`
- `oled:clear(color)`
- `oled:pixel(x, y, color)`
- `oled:fill_rect(x, y, w, h, color)`
- `oled:draw_char(x, y, ch, color)`
- `oled:draw_text(x, y, text, color)`
- `oled:invert(enable)`
- `oled:contrast(value)`
- `oled:show()`
- `oled:close()`

`color` is truthy for lit pixels and `false`/`nil` for cleared pixels.

## Example
```lua
local i2c = require("i2c")
local ssd1306 = dofile("/fatfs/scripts/ssd1306.lua")

local bus = i2c.new(0, 14, 13, 400000)
local dev = bus:device(0x3C)
local oled = ssd1306.new(dev, {
    width = 128,
    height = 64,
    addr = 0x3C,
})

oled:init()
oled:clear(false)
oled:draw_text(10, 10, "SSD1306 OK", true)
oled:show()

oled:close()
dev:close()
bus:close()
```

## Builtin demo
Run the self-test script with:

```text
lua --run --path builtin/basic_ssd1306.lua --timeout-ms 10000
```
