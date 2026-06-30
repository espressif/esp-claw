# Lua Fuel Gauge

This skill describes how to read battery fuel-gauge data from Lua using the existing `i2c` module.
When a request mentions `fuel gauge`, `battery voltage`, `battery current`, or `battery percentage`, use this module by default.

The module supports multiple fuel-gauge ICs through a chip profile architecture.
Currently supported chips: **BQ27220**, **MAX17048** (voltage + SOC only, no current).

## How to call
- Import it with `local fuel_gauge = require("lib_fuel_gauge")`
- Create a gauge with `local gauge = fuel_gauge.new({ bus = bus })` (defaults to BQ27220)
- Select a specific chip with `local gauge = fuel_gauge.new({ bus = bus, chip = "max17048" })`
- Read all values with `local sample = gauge:read()`
- Or read one value at a time:
  - `gauge:read_voltage_mv()`
  - `gauge:read_current_ma()` (not available on all chips)
  - `gauge:read_soc()`
- Query the active chip with `gauge:chip()`
- List supported chips with `fuel_gauge.supported_chips()`
- Call `gauge:close()` when needed

## Options table
| Field      | Type    | Meaning                                              |
|------------|---------|------------------------------------------------------|
| `chip`     | string  | Chip model name, e.g. `"bq27220"`, `"max17048"`     |
| `port`     | integer | I2C port number                                      |
| `sda`      | integer | SDA GPIO number                                      |
| `scl`      | integer | SCL GPIO number                                      |
| `freq_hz`  | integer | I2C clock in Hz (default `400000`)                   |
| `frequency`| integer | Alias of `freq_hz`                                   |
| `addr`     | integer | 7-bit I2C address (default depends on chip)          |
| `bus`      | userdata| Existing `i2c` bus handle, recommended               |

## Data format
- `sample.chip` — chip name string
- `sample.voltage_mv`
- `sample.current_ma` (nil when the chip has no current register)
- `sample.soc`

## Example
```lua
local fuel_gauge = require("lib_fuel_gauge")
local i2c = require("i2c")

local bus = i2c.new(0, 14, 13, 400000)
local gauge = fuel_gauge.new({
    bus = bus,
    chip = "bq27220",   -- or "max17048"
})

print("chip:", gauge:chip())
local sample = gauge:read()
print(sample.voltage_mv, sample.current_ma, sample.soc)
gauge:close()
bus:close()
```
