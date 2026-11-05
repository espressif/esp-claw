# Lua GPIO

This module describes how to correctly use gpio when writing Lua scripts.

## How to call
- Import it with `local gpio = require("gpio")`
- Call `gpio.set_direction(pin, mode)` to set pin mode
- Call `gpio.set_level(pin, level)` to set output level
- Call `gpio.get_level(pin)` to read pin level
- Call `gpio.close(pin)` to reset and release the pin; repeated close is safe

Supported modes are `"input"`, `"output"`, `"input_output"`, `"output_od"`, `"input_output_od"`, and `"disable"`.

`set_direction` and `set_level` return no values on success and raise a Lua error on failure. `get_level` returns `0` or `1`.

A pin cannot be used while another device or script owns it. Call `gpio.close(pin)` to release it; otherwise it remains in use until the job exits. Garbage collection alone does not release pins. Retry a failed close before reusing the pin.

The example uses GPIO4. Check that it is available and output-capable on your board.

## Example
```lua
local gpio = require("gpio")
gpio.set_direction(4, "output")
gpio.set_level(4, 1)
gpio.close(4)
```
