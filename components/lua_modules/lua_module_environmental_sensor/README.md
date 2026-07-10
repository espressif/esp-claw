# Lua Environmental Sensor

This module describes how to read environmental data from Lua using one of
multiple backends selected at build time and at runtime. It currently
supports Bosch BME690 and DHT-family sensors.

## How to call
- Import it with `local environmental_sensor = require("environmental_sensor")`
- Open the sensor with one of:
  - `local sensor = environmental_sensor.new()` — default to the compiled BME690 backend
  - `local sensor = environmental_sensor.new("environmental_sensor")` — choose a BME690 board device explicitly
  - `local sensor = environmental_sensor.new({ type = "bme690", peripheral = "i2c_master" })`
  - `local sensor = environmental_sensor.new({ type = "dht", pin = 4, sensor_type = "dht22" })`
- Read all values with `local sample = sensor:read()`
- Or read one value at a time:
  - `sensor:read_temperature()`
  - `sensor:read_humidity()`
- BME690-only helpers:
  - `sensor:read_pressure()`
  - `sensor:read_gas()`
- Inspect sensor identity with:
  - `sensor:name()`
- BME690-only identity helpers:
  - `sensor:chip_id()`
  - `sensor:variant_id()`
- Call `sensor:close()` when needed

## Options table
All fields are optional unless the chosen backend requires them.

| Field              | Type    | Meaning                                                         |
|--------------------|---------|-----------------------------------------------------------------|
| `type`             | string  | Backend type: `"bme690"` or `"dht"`                             |
| `device`           | string  | Board device name to load defaults from                         |
| `peripheral`       | string  | Board I2C master peripheral name, e.g. `"i2c_master"`          |
| `i2c_addr`         | integer | BME690 7-bit I2C address, `0x76` or `0x77`                     |
| `frequency`        | integer | I2C clock in Hz (default `400000`)                             |
| `heater_temp`      | integer | Heater target temperature in Celsius (default `300`)           |
| `heater_duration`  | integer | Heater duration in milliseconds (default `100`)                |
| `pin`              | integer | DHT GPIO number                                                 |
| `sensor_type`      | string  | DHT subtype: `"dht11"`, `"dht22"`, `"si7021"`, and aliases     |

## Data format
- Common:
  - `sample.temperature` — degrees Celsius
  - `sample.humidity` — relative humidity in percent
- BME690-only:
  - `sample.pressure` — pressure in Pa
  - `sample.gas_resistance` — gas resistance in ohms
  - `sample.status` — raw BME690 status flags
  - `sample.gas_index` — gas measurement index from the driver
  - `sample.meas_index` — measurement index from the driver

## Example: BME690
```lua
local environmental_sensor = require("environmental_sensor")

local sensor = environmental_sensor.new()
local sample = sensor:read()

print(string.format("temperature: %.2f C", sample.temperature))
print(string.format("pressure: %.2f Pa", sample.pressure))
print(string.format("humidity: %.2f %%", sample.humidity))
print(string.format("gas resistance: %.2f ohm", sample.gas_resistance))
print(string.format("status: 0x%02X", sample.status))

sensor:close()
```

## Example: DHT
```lua
local environmental_sensor = require("environmental_sensor")

local sensor = environmental_sensor.new({
    type = "dht",
    pin = 4,
    sensor_type = "dht22",
})

local sample = sensor:read()
print(string.format("temperature: %.2f C", sample.temperature))
print(string.format("humidity: %.2f %%", sample.humidity))

sensor:close()
```

## Notes
- Reads are blocking.
- Each BME690 call triggers a forced-mode measurement.
- The BME690 board device must resolve to a valid I2C peripheral, or you must
  pass `peripheral` explicitly in Lua.
- Any setup or read failure raises a Lua error.
