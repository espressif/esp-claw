# Lua Environmental Sensor

`environmental_sensor` reads BME690 temperature, humidity, pressure and gas
resistance, or DHT-family temperature and humidity. Enable the required backend
in the firmware configuration.

## Create a sensor

`environmental_sensor.new(opts)` accepts one options table. Unknown options,
invalid values, resource conflicts and initialization failures raise Lua errors.

### BME690

```lua
local i2c = require("i2c")
local environmental_sensor = require("environmental_sensor")
local bus = i2c.borrow(0) -- An initialized board I2C controller.
local sensor = environmental_sensor.new({type = "bme690", bus = bus, address = 0x76})
local ok, result = pcall(function() return sensor:read() end)
sensor:close()
bus:close()
assert(ok, result)
print(result.temperature, result.humidity, result.pressure)
```

Options:

| Field | Meaning |
|---|---|
| `type` | Required: `"bme690"`. |
| `bus` | Required: an open bus from `i2c.new(...)` or `i2c.borrow(port)`. |
| `address` | `0x76` (default) or `0x77`. |
| `frequency` | I2C clock in Hz, `1..400000`; default `400000`. |
| `heater_temp` | Heater temperature in Celsius, `0..400`; default `300`. |
| `heater_duration` | Heater duration in milliseconds, `0..4032`; default `100`. |

The sensor exclusively occupies its I2C address and keeps the bus open until
closed. Close the sensor before closing the bus. A borrowed board bus must
remain initialized while in use. This constructor does not accept Board Manager
device names; use an explicit bus and address for an available sensor.

### DHT

```lua
local environmental_sensor = require("environmental_sensor")
local sensor = environmental_sensor.new({type = "dht", pin = 4, sensor_type = "dht22"})
local ok, sample = pcall(function() return sensor:read() end)
sensor:close()
assert(ok, sample)
print(sample.temperature, sample.humidity)
```

Choose a free GPIO that supports both input and output and is suitable for your
board wiring. The module occupies that GPIO until close.

- `type`: required, `"dht"`.
- `pin`: required GPIO number.
- `sensor_type`: defaults to `"dht11"`; also accepts `"dht22"`, `"dht21"`,
  `"am2301"`, `"am2302"`, `"am2321"` and `"si7021"` (DHT-compatible protocol).

Use the pull-up and sampling interval required by the sensor. Each read includes
an approximately 200 ms delay; this does not replace the sensor's minimum interval
between samples. A two-second interval is suitable for the DHT11/DHT22 examples.

## Methods

All reads are synchronous and raise Lua errors on failure. BME690 reads start a
new measurement and wait for conversion and heating. Each convenience read also
starts a separate measurement; use `read()` for a combined sample.

| Method | Return value |
|---|---|
| `sensor:read()` | Table containing `temperature` (Celsius) and `humidity` (%RH). BME690 also returns `pressure` (Pa), `gas_resistance` (ohms), `status`, `gas_index` and `meas_index`. |
| `sensor:read_temperature()` | Temperature in Celsius. |
| `sensor:read_humidity()` | Relative humidity in %RH. |
| `sensor:read_pressure()` | BME690 only: pressure in Pa. |
| `sensor:read_gas()` | BME690 only: gas resistance in ohms. |
| `sensor:chip_id()` | BME690 only: chip ID. |
| `sensor:variant_id()` | BME690 only: variant ID. |
| `sensor:read_raw()` | DHT only: temperature and humidity as integers in tenths of their units. |
| `sensor:name()` | `"bme690"` or `"dht"`. |
| `sensor:close()` | Releases the device's resources; no return value. Repeated successful close is harmless. |

BME690 `status` is the sensor status byte; gas-valid (`0x20`) and heater-stable
(`0x10`) bits indicate whether the gas result is ready for use. Gas resistance is
not an air-quality index.

Do not operate the same sensor through another driver while it is open. Close
explicitly when finished. Garbage collection also attempts cleanup; after a
close error, retry close before reusing the resources. Other methods reject a
closed sensor.
