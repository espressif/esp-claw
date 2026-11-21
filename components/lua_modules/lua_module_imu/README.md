# Lua IMU

`imu` reads raw acceleration, angular velocity, temperature, and interrupt status from supported IMUs. Available chips are BMI270, ICM42670, and MPU6050, depending on the firmware configuration.

## Board sensor

```lua
local imu = require("imu")
local sensor = imu.new({ device = "imu_sensor" })
local sample = sensor:read()
print(sample.accel.x, sample.gyro.z)
sensor:close()
```

`device` selects an IMU supported by the board firmware. It is initialized automatically if needed. Existing sampling settings are preserved, and closing the Lua handle leaves the board sensor available. A second Lua handle cannot open the same board IMU until the first closes.

## External sensor

```lua
local i2c = require("i2c")
local imu = require("imu")
local bus = i2c.new(1, 4, 5)
local sensor = imu.new({ chip = "mpu6050", bus = bus, address = 0x68 })
print(sensor:read().accel.z)
sensor:close()
bus:close()
```

Check your board before choosing a controller or GPIOs. An external sensor can also use an `i2c.borrow(port)` bus handle.

| Field | Description |
| --- | --- |
| `chip` | Required: `bmi270`, `icm42670`, or `mpu6050`; the chip must be enabled in the firmware |
| `bus` | Required Lua I2C bus handle |
| `address` | Optional 7-bit address; defaults to the chip's default address |
| `int_gpio` | Optional interrupt input GPIO; defaults to `-1` (unused) |
| `sdo_gpio` | Optional address strap output GPIO; defaults to `-1` (unused) |

External sensors use 400 kHz I2C transfers. Their address and optional GPIOs must be unused. Close the sensor before closing its bus: the bus cannot close while a sensor uses it.

Pass exactly one configuration table. Board mode accepts only `device`; it cannot be combined with external sensor fields. Unknown options, unavailable devices, unsupported chips, and resource conflicts raise Lua errors.

## Methods and cleanup

- `sensor:read()` returns `accel` and `gyro` tables, each containing `x`, `y`, and `z`, plus `sens_time` and `status`.
- `sensor:read_temperature()` returns the chip's raw temperature value.
- `sensor:read_int_status()` returns the chip's interrupt status bits; reading may clear hardware status bits.
- `sensor:name()` returns the board device name or, for an external sensor, the chip name.
- `sensor:close()` releases the Lua handle and destroys external sensor instances. Repeated successful closes are harmless.

Values are raw chip-specific readings, not normalized physical units. BMI270 `sens_time` is the sensor counter; ICM42670 and MPU6050 use system microseconds. Board sensor ranges follow board configuration; external sensor ranges depend on the selected chip.

Read methods raise errors after closing or when hardware access fails. Use `pcall` to handle errors. Lua garbage collection also finalizes handles, but explicit close is recommended. Retry a failed close before dropping the handle.
