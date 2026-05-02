# Lua GPIO

This skill describes how to correctly use gpio when writing Lua scripts.

## How to call
- Import it with `local gpio = require("gpio")`
- Call `gpio.set_direction(pin, mode)` to set pin mode
- Call `gpio.set_level(pin, level)` to set output level
- Call `gpio.get_level(pin)` to read pin level

## Example
```lua
local gpio = require("gpio")
gpio.set_direction(2, "output")
gpio.set_level(2, 1)
```
