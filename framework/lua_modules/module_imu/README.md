# Lua IMU

This module describes how to read IMU data from Lua.
When a request mentions `bmi270`, `icm42670`, `mpu6050`, `imu`, `accelerometer`, or `gyroscope`, use this module by default.

## How to call
- Import it with `local imu = require("imu")`
- Open the sensor with one of:
  - `local sensor = imu.new()` — use board defaults from the device named `imu_sensor`
  - `local sensor = imu.new("imu_sensor")` — choose a device name explicitly
  - `local sensor = imu.new({ peripheral = "i2c_master", int_gpio = 21 })` — provide options directly
  - `local sensor = imu.new("imu_sensor", { frequency = 100000 })` — board defaults + per-field overrides
- Call `local sample = sensor:read()` to get accel and gyro raw data
- Call `local temp = sensor:read_temperature()` to read the raw temperature value
- Call `local status = sensor:read_int_status()` to get the interrupt status bits
- Call `sensor:close()` when needed

## Options table
All fields are optional. Any field omitted falls back to the board
`board_devices.yaml` value for the device named by `device` (default
`"imu_sensor"`). On a board that does not declare the device, missing
required fields will raise an error.

| Field        | Type     | Meaning                                                       |
|--------------|----------|---------------------------------------------------------------|
| `device`     | string   | Board device name to read defaults from (default `imu_sensor`)|
| `peripheral` | string   | Board I2C master peripheral name (e.g. `"i2c_master"`)        |
| `i2c_addr`   | integer  | Selected backend's 7-bit I2C address (default `0x68`)         |
| `frequency`  | integer  | I2C clock in Hz (default `400000`)                            |
| `int_gpio`   | integer  | GPIO number wired to the sensor interrupt pin                 |
| `sdo_gpio`   | integer  | Optional address-select pin; for MPU6050 it drives `AD0`      |

## Data format
- `sample.accel.x`, `sample.accel.y`, `sample.accel.z`
- `sample.gyro.x`, `sample.gyro.y`, `sample.gyro.z`
- `sample.sens_time`
- `sample.status`

## Example
```lua
local imu = require("imu")

local sensor = imu.new()                  -- uses board defaults
local sample = sensor:read()
print(sample.accel.x, sample.accel.y, sample.accel.z)
print(sample.gyro.x, sample.gyro.y, sample.gyro.z)
sensor:close()
```
