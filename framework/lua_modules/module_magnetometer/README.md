# Lua Magnetometer

This skill describes how to read magnetometer data from Lua.
When a request mentions `bmm350`, `bmm150`, `magnetometer`, `magnetic field`, or `compass`,
use this module by default. The active chip backend is selected at build time via
`CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_*` (BMM350, BMM150, or QMC6309).

## How to call
- Import it with `local magnetometer = require("magnetometer")`
- Open the sensor with one of:
  - `local sensor = magnetometer.new()` — use board defaults from `magnetometer_sensor`
  - `local sensor = magnetometer.new("magnetometer_sensor")` — choose a device name explicitly
  - `local sensor = magnetometer.new({ peripheral = "i2c_master" })` — provide options directly
  - `local sensor = magnetometer.new("magnetometer_sensor", { i2c_addr = 0x15 })` — board defaults + per-field overrides
- Call `local sample = sensor:read()` to get magnetic field data and temperature
- Call `local temp = sensor:read_temperature()` to read temperature
- Call `local status = sensor:read_int_status()` to get the raw interrupt status register
- Call `sensor:calibration_reset()` before collecting calibration samples
- Call `sensor:calibration_add_sample()` repeatedly while rotating the device in all directions
- Call `local cal = sensor:calibration_finish()` to compute and persist hard/soft iron calibration
- Call `local cal = sensor:calibration_get()` to inspect the active calibration
- Call `sensor:calibration_clear()` to clear persisted calibration
- Call `sensor:close()` when needed

## Options table
All fields are optional. Any field omitted falls back to the board
`board_devices.yaml` value for the device named by `device` (default
`"magnetometer_sensor"`). On a board that does not declare the device,
missing required fields will raise an error.

| Field        | Type    | Meaning                                                               |
|--------------|---------|-----------------------------------------------------------------------|
| `device`     | string  | Board device name to read defaults from (default `magnetometer_sensor`) |
| `peripheral` | string  | Board I2C master peripheral name (e.g. `"i2c_master"`)               |
| `i2c_addr`   | integer | 7-bit I2C address (BMM350: `0x14`/`0x15`; BMM150: `0x10`–`0x13`; QMC6309: `0x7C`) |
| `frequency`  | integer | I2C clock in Hz (default `100000`)                                   |
| `int_gpio`   | integer | Optional GPIO number wired to the sensor interrupt pin               |
| `sdo_gpio`   | integer | Optional GPIO to strap SDO/ADSEL for alternate I2C address           |

## Data format
- `sample.magnetic.x`, `sample.magnetic.y`, `sample.magnetic.z`
- `sample.raw_magnetic.x`, `sample.raw_magnetic.y`, `sample.raw_magnetic.z`
- `sample.temperature` (BMM150 and QMC6309 return `0`; no on-chip temperature)
- `sample.status`
- `sample.calibrated`

## Calibration
The module applies a simple hard-iron plus diagonal soft-iron calibration,
matching the approach used by the reference compass app.

Suggested flow:
```lua
sensor:calibration_reset()
for i = 1, 500 do
  sensor:calibration_add_sample()
end
local cal = sensor:calibration_finish()
print(cal.calibrated, cal.sample_count)
```

## Example
```lua
local magnetometer = require("magnetometer")

local sensor = magnetometer.new()
local sample = sensor:read()
print(sample.magnetic.x, sample.magnetic.y, sample.magnetic.z)
print(sample.temperature)
sensor:close()
```
