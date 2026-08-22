# Lua GPIO

This module describes how to correctly use gpio when writing Lua scripts.

## How to call
- Import it with `local gpio = require("gpio")`
- Call `gpio.set_direction(pin, mode)` to set pin mode
- Call `gpio.set_level(pin, level)` to set output level
- Call `gpio.get_level(pin)` to read pin level

Supported modes are `"input"`, `"output"`, `"input_output"`, `"output_od"`, `"input_output_od"`, and `"disable"`.

`set_direction` and `set_level` return no values on success and raise a Lua error on failure. `get_level` returns `0` or `1`.

## Example
```lua
local gpio = require("gpio")
gpio.set_direction(2, "output")
gpio.set_level(2, 1)
```
