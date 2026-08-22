# Lua ADC (one-shot)

This module describes how to read a calibrated voltage in millivolts from an ADC-capable GPIO in Lua.

## How to call
- Import it with `local adc = require("adc")`
- Create a channel with `local ch = adc.new(gpio)`
  - `gpio`: a GPIO number wired to an ADC-capable pad. ADC unit and
    channel are resolved automatically from the GPIO. If the chip does not
    support on-chip calibration, `new()` raises a Lua error — wrap in
    `pcall` if you want to handle that gracefully.
- `ch:read()` -> current voltage in millivolts as an integer.
- `ch:get_gpio()` → the GPIO number this channel is bound to.
- `ch:close()` when you're done. Handles are also cleaned up on garbage
  collection, but explicit `close()` is preferred for determinism.

## Example: read a potentiometer
```lua
local adc = require("adc")
local delay = require("delay")

local ch = adc.new(4)   -- GPIO 4
for _ = 1, 5 do
    print(string.format("%d mV", ch:read()))
    delay.delay_ms(200)
end
ch:close()
```

## Notes
- `ch:read()` is a blocking single-sample read.
- Multiple channels can coexist; call `adc.new()` for each GPIO.
