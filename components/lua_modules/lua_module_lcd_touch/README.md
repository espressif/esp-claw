# Lua LCD Touch

This module describes how to correctly use lcd_touch when writing Lua scripts.

## How to call
- Import it with `local lcd_touch = require("lcd_touch")`
- Get a touch handle first, typically from `board_manager.get_lcd_touch_handle(...)`
- Call `lcd_touch.sync(touch_handle)` to synchronize the cached touch state
- Call `lcd_touch.read(touch_handle)` to read the current touch state
- Call `lcd_touch.poll(touch_handle)` to get edge-aware touch information such as `just_pressed`, `just_released`, `x`, `y`, `dx`, `dy`, and `held_ms`

## Example
```lua
local board_manager = require("board_manager")
local lcd_touch = require("lcd_touch")

local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")
lcd_touch.sync(touch_handle)
local touch = lcd_touch.poll(touch_handle)
if touch.just_pressed then
  print(touch.x, touch.y)
end
```
