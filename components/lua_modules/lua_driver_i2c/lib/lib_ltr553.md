# lib_ltr553.lua

Reusable Lua driver for the Lite-On LTR-553ALS combined ambient light and proximity sensor. It uses the builtin `i2c` module and exports `require("lib_ltr553")`.

## When to use

Use this library when a script needs ambient light in lux, raw visible/infrared counts, or the built-in proximity reading. This is the light sensor fitted to M5Stack CoreS3 (and therefore StackChan), on the internal I2C bus at the part's fixed address `0x23`.

## Loading

```lua
local ltr553 = require("lib_ltr553")
```

The script must also have access to the `i2c` and `delay` modules. You can either pass an existing I2C bus handle or let the library create one from GPIO options.

## Constructor

```lua
local sensor = ltr553.new(opts)
```

`opts` is a table:

- `bus`: existing I2C bus userdata. Recommended when the script already owns a bus, or when the board manager already created it.
- `port`: I2C port number. Required if `bus` is not provided.
- `sda`: SDA GPIO. Required if `bus` is not provided.
- `scl`: SCL GPIO. Required if `bus` is not provided.
- `freq_hz`: I2C frequency in Hz. Defaults to `400000`.
- `frequency`: alias of `freq_hz`.
- `addr`: 7-bit I2C address. Defaults to `0x23`; the part is not strappable, so there is normally no reason to change this.
- `close_bus`: set to `true` to let `sensor:close()` also close a bus the library created.
- `gain`: ALS gain multiplier. One of `1`, `2`, `4`, `8`, `48`, `96`. Defaults to `1`.
- `integration_time_ms`: one of `50`, `100`, `150`, `200`, `250`, `300`, `350`, `400`. Defaults to `100`.
- `measurement_rate_ms`: ALS repeat rate, one of `50`, `100`, `200`, `500`, `1000`, `2000`. Defaults to `500`.
- `enable_als`: set to `false` to leave the ALS in standby. Defaults to enabled.
- `enable_ps`: set to `true` to also run the proximity sensor. Defaults to disabled.
- `ps_gain`: proximity gain multiplier, one of `16`, `32`, `64`. Defaults to `16`.
- `ps_pulses`: proximity LED pulse count, `1`..`15`.
- `ps_measurement_rate_ms`: one of `10`, `50`, `70`, `100`, `200`, `500`, `1000`, `2000`.
- `ps_saturation_indicator`: set to `false` to disable the saturation flag in the proximity data register. Defaults to enabled, because `read_proximity()`'s saturation return value is meaningless without it.
- `check_id`: set to `false` to skip the identity check. By default the constructor errors out unless the `PART_ID` part-number field (bits 7:4) is `0x9` and `MANUFAC_ID` is `0x05`.

`integration_time_ms` must be less than or equal to `measurement_rate_ms`. The part silently clamps a longer integration time down to the repeat rate, which would make readings disagree with the lux divisor, so the library rejects that combination instead.

Only the documented human values are accepted for the timing and gain options; raw register codes are rejected because several of them collide with valid millisecond values.

## Module members

- `ltr553.GAIN`, `ltr553.PS_GAIN`, `ltr553.INTEGRATION_TIME_MS`, `ltr553.MEASUREMENT_RATE_MS`, `ltr553.PS_MEASUREMENT_RATE_MS`: maps from human values to register codes.
- `ltr553.compute_lux(ch0, ch1, gain, integration_time_ms)`: the lux equation, exposed for scripts that keep their own raw counts.

## Methods

### Identity and configuration

- `sensor:address()`: returns the configured 7-bit I2C address.
- `sensor:part_id()`: returns the part-number field (bits 7:4, `0x9`) and the revision field (bits 3:0).
- `sensor:manufacturer_id()`: returns the manufacturer ID byte, `0x05` on a genuine part.
- `sensor:gain()` / `sensor:integration_time_ms()` / `sensor:measurement_rate_ms()` / `sensor:ps_gain()`: returns the values currently in use.
- `sensor:set_gain(gain)`
- `sensor:set_timing(integration_time_ms, measurement_rate_ms)`: either argument may be `nil` to leave it alone. Raises an error if the resulting integration time exceeds the repeat rate, leaving the previous setting in place.
- `sensor:set_als_enabled(enabled)` / `sensor:set_ps_enabled(enabled)`
- `sensor:set_ps_gain(gain)`: `16`, `32` or `64`.
- `sensor:set_ps_pulses(count)`: `1`..`15`.
- `sensor:set_ps_measurement_rate(rate_ms)`
- `sensor:set_ps_led_raw(value)`: raw `PS_LED` register write for the emitter's current, duty and frequency fields. See the datasheet for the layout.
- `sensor:reset()`: software reset, then re-applies the current configuration.

### Measurements

- `sensor:read_raw()`: returns `ch0, ch1`. `ch0` is visible + infrared, `ch1` is infrared only.
- `sensor:read_lux()`: returns `lux, ch0, ch1`.
- `sensor:read_proximity()`: returns the 11-bit proximity count and a boolean saturation flag.
- `sensor:status()`: returns a table with `raw`, `ps_new_data`, `ps_interrupt`, `als_new_data`, `als_interrupt`, `gain_code` and `data_invalid`.
- `sensor:read()`: returns a table with `lux`, `ch0`, `ch1`, `als_new_data`, `ps_new_data`, `data_invalid`, plus `proximity` and `proximity_saturated` when the proximity sensor is enabled.
- `sensor:close()`: releases the I2C device handle, and the bus too when the library created it and `close_bus` was set.

## Example: read ambient light on CoreS3 / StackChan

```lua
local i2c = require("i2c")
local ltr553 = require("lib_ltr553")
local delay = require("delay")

-- CoreS3 internal I2C bus; attaching to the port the board manager already
-- owns is safe.
local bus = i2c.new(0, 12, 11, 400000)
local sensor = ltr553.new({ bus = bus, measurement_rate_ms = 100 })

for _ = 1, 5 do
    local sample = sensor:read()
    print(string.format("%.1f lux (ch0=%d ch1=%d)", sample.lux, sample.ch0, sample.ch1))
    delay.delay_ms(200)
end

sensor:close()
bus:close()
```

## Notes

- Lux comes from the Lite-On "Using the Lux Equation" application note. `RATIO = ch1 / (ch0 + ch1)` selects one of three linear segments; a ratio of `0.85` or higher is outside the characterised range and the note specifies a result of zero, so `read_lux()` returns `0` there.
- The ALS needs about 10 ms after leaving standby before it produces data, and one full `integration_time_ms` before the first sample is meaningful. The library waits the wakeup delay but does not wait out the first integration.
- Reading faster than `measurement_rate_ms` returns the same counts. Check `status().als_new_data` if a script needs to know whether a sample is fresh.
- Higher gain settings clip in bright light. If `ch0` pins near `0xFFFF`, drop the gain.
