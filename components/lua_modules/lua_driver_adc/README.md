# Lua ADC (one-shot)

This module describes how to read raw samples and calibrated millivolts from an ADC-capable GPIO in Lua.

## How to call
- Import it with `local adc = require("adc")`
- Create a channel with `local ch = adc.new(gpio[, config])`
  - `gpio`: a GPIO number wired to an ADC-capable pad. ADC unit and
    channel are resolved automatically from the GPIO.
  - `config.atten`: optional attenuation constant. The default is
    `adc.ATTEN_DB_0`.
  - `config.bitwidth`: optional bit-width constant. The default is
    `adc.BITWIDTH_DEFAULT`; unsupported widths raise an error.
- `ch:read_raw()` -> current raw ADC sample as an integer.
- `ch:read_mv()` -> calibrated voltage in millivolts. It raises an error if
  calibration is unavailable on the chip.
- `ch:is_calibrated()` -> whether calibrated millivolt reads are available.
- `ch:get_config()` -> a table containing `gpio`, `atten`, and the resolved
  `bitwidth` used by the chip.
- Call `ch:close()` when done, or use Lua's `<close>` variables. Unreachable handles are finalized by Lua GC; job exit closes remaining handles. Retry a failed explicit close before dropping the handle.

The module exports `ATTEN_DB_0`, `ATTEN_DB_2_5`, `ATTEN_DB_6`, `ATTEN_DB_12`,
`BITWIDTH_DEFAULT`, and `BITWIDTH_9` through `BITWIDTH_13`.

## Example: read a potentiometer
```lua
local adc = require("adc")
local delay = require("delay")

local ch <close> = adc.new(4, { atten = adc.ATTEN_DB_0 })
for _ = 1, 5 do
    print(string.format("raw=%d", ch:read_raw()))
    delay.delay_ms(200)
end
```

## Notes
- Read methods perform a blocking single-sample read.
- Multiple channels can coexist; call `adc.new()` for each GPIO.
- Opening the same GPIO while an existing handle is active raises an error.
- Config only accepts `atten` and `bitwidth`; unknown fields raise an error.
