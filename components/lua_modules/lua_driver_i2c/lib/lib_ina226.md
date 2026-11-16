# lib_ina226.lua

Reusable Lua driver for the TI INA226 bidirectional current, voltage and power monitor. It uses the builtin `i2c` module and exports `require("lib_ina226")`.

## When to use

Use this library when a script needs to measure a rail's voltage and the current flowing through a shunt resistor. On M5Stack StackChan the part sits on the CoreS3 internal I2C bus at address `0x41` with a 10 mΩ shunt, and reports battery voltage and battery current: current is **positive while discharging** and negative while charging.

## Loading

```lua
local ina226 = require("lib_ina226")
```

The script must also have access to the `i2c` and `delay` modules. You can either pass an existing I2C bus handle or let the library create one from GPIO options.

## Constructor

```lua
local monitor = ina226.new(opts)
```

`opts` is a table:

- `bus`: existing I2C bus userdata. Recommended when the script already owns a bus, or when the board manager already created it.
- `port`: I2C port number. Required if `bus` is not provided.
- `sda`: SDA GPIO. Required if `bus` is not provided.
- `scl`: SCL GPIO. Required if `bus` is not provided.
- `freq_hz`: I2C frequency in Hz. Defaults to `400000`.
- `frequency`: alias of `freq_hz`.
- `addr`: 7-bit I2C address, `0x40`..`0x4F`. Defaults to `0x40`.
- `close_bus`: set to `true` to let `monitor:close()` also close a bus the library created.
- `shunt_res`: **required.** Shunt resistance in ohms.
- `max_expected_current`: **required.** Full-scale current in amps. This sets the resolution of the current and power readings.
- `averaging`: samples averaged per conversion. One of `1`, `4`, `16`, `64`, `128`, `256`, `512`, `1024`. Defaults to `16`.
- `bus_conversion_time_us`: one of `140`, `204`, `332`, `588`, `1100`, `2116`, `4156`, `8244`. Defaults to `1100`.
- `shunt_conversion_time_us`: same set as above. Defaults to `1100`.
- `mode`: `"power_down"`, `"shunt_triggered"`, `"bus_triggered"`, `"shunt_bus_triggered"`, `"shunt_continuous"`, `"bus_continuous"` or `"shunt_bus_continuous"`. Defaults to `"shunt_bus_continuous"`.
- `check_id`: set to `false` to skip the identity check. By default the constructor errors out unless the manufacturer-id register `0xFE` reads `0x5449` (`"TI"`). `check_die_id` is accepted as an alias for this option.

The identity gate is the **manufacturer** id, not the die id. Some parts M5Stack ships — including the one at `0x41` on StackChan — return the manufacturer id from the die-id register `0xFF` as well, instead of the textbook `0x2260`. Gating on the die id would reject a chip that works correctly. The die id is still read and exposed so a script can notice a non-standard part.

`shunt_res` and `max_expected_current` are deliberately not defaulted: a wrong shunt value produces readings that look plausible but are wrong by a constant factor.

## Module tables

- `ina226.AVERAGING`: maps sample counts to register codes.
- `ina226.CONVERSION_TIME_US`: maps microsecond values to register codes.
- `ina226.MODE`: maps mode names to register codes.

Only the values in these tables are accepted; raw register codes are rejected because `1` and `4` are both valid sample counts and valid codes.

## Methods

- `monitor:address()`: returns the configured 7-bit I2C address.
- `monitor:manufacturer_id()`: reads register `0xFE`, `0x5449` on a TI part.
- `monitor:die_id()`: reads register `0xFF`, `0x2260` on a textbook INA226.
- `monitor:die_id_is_standard()`: whether the die id read at construction was `0x2260`. `false` is not a fault — see the note above.
- `monitor:read_bus_voltage()`: volts on the VBUS pin, 1.25 mV resolution.
- `monitor:read_shunt_voltage()`: volts across the shunt, 2.5 µV resolution, signed.
- `monitor:read_current()`: amps through the shunt, signed.
- `monitor:read_power()`: watts. Unsigned, so this never carries a sign.
- `monitor:read()`: returns a table with `bus_voltage`, `shunt_voltage`, `current` and `power`.
- `monitor:current_lsb()`: amps per current-register count.
- `monitor:calibration()`: the calibration register value in use.
- `monitor:configure(cfg)`: re-apply any subset of the configuration fields above.
- `monitor:wait_conversion_ready([timeout_ms])`: polls the conversion-ready flag and returns `true` when set, `false` on timeout (default 100 ms). Only needed after a triggered-mode write; in continuous mode the registers are always readable.
- `monitor:close()`: releases the I2C device handle, and the bus too when the library created it and `close_bus` was set.

## Example: StackChan battery voltage and current

```lua
local i2c = require("i2c")
local ina226 = require("lib_ina226")

-- CoreS3 internal I2C bus; attaching to the port the board manager already
-- owns is safe.
local bus = i2c.new(0, 12, 11, 400000)

local monitor = ina226.new({
    bus = bus,
    addr = 0x41,
    shunt_res = 0.01,
    max_expected_current = 8.19,
})

local sample = monitor:read()
print(string.format("%.3f V  %.3f A  %.3f W",
    sample.bus_voltage, sample.current, sample.power))

if sample.current < 0 then
    print("charging")
else
    print("discharging")
end

monitor:close()
bus:close()
```

## Notes

- The calibration register is computed as `0.00512 / (current_lsb * shunt_res)` with `current_lsb = max_expected_current / 32768`. The constructor errors out if that lands outside `1..65535`, which is the usual sign of a mistyped shunt value.
- Config bits 14:12 are reserved and specified as `0b100`; the library sets them so a read-back of the config register matches what was written.
- Total conversion time is roughly `averaging * (bus_conversion_time + shunt_conversion_time)`. The defaults give about 35 ms per updated reading, so polling faster than that returns the same numbers.
