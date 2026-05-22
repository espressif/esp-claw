# Lua ADC (one-shot)

This skill describes how to read a voltage (in millivolts) from an
ADC-capable GPIO in Lua. It wraps ESP-IDF's `esp_adc` one-shot driver and
the chip's built-in calibration, and only exposes calibrated voltage
readings — callers don't need to think about attenuation, bit-width, or
raw ADC codes.

## How to call
- Import it with `local adc = require("adc")`
- Create a channel with `local ch = adc.new(gpio)`
  - `gpio`: a GPIO number wired to an ADC-capable pad. ADC unit and
    channel are resolved automatically from the GPIO. If the chip does not
    support on-chip calibration, `new()` raises a Lua error — wrap in
    `pcall` if you want to handle that gracefully.
- `ch:read()` → current voltage in millivolts (integer). Blocking, returns
  within microseconds.
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
- `ch:read()` is blocking on the order of microseconds. For fast streaming
  or high-rate sampling, the one-shot driver is not the right tool.
- Multiple channels can coexist; just call `adc.new()` for each GPIO.
