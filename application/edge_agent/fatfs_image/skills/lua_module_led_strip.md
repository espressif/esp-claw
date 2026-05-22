# Lua LED Strip

This skill describes how to correctly use led_strip when writing Lua scripts.
When a request mentions `ws2812`, use this `led_strip` module by default.

## How to call
- Import it with `local led_strip = require("led_strip")`
- Call `local strip = led_strip.new(gpio, max_leds)` to create a strip handle
- Call `strip:set_pixel(index, r, g, b)` to set one pixel
- Call `strip:set_pixel_hsv(index, h, s, v)` to set one pixel using HSV
- Call `strip:refresh()` to apply changes
- Call `strip:clear()` or `strip:close()` when needed

`set_pixel_hsv` uses:
- `h`: `0-359`
- `s`: `0-255`
- `v`: `0-255`

## Example
```lua
local led_strip = require("led_strip")

local strip = led_strip.new(8, 1)
strip:set_pixel(0, 255, 0, 0)
strip:set_pixel_hsv(0, 120, 255, 64)
strip:refresh()
```
