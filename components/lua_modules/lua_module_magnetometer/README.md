# Lua Magnetometer

`magnetometer` reads magnetic field samples from BMM150, BMM350, and QMC6309 sensors. Each chip must be enabled in the firmware.

## Board sensor

```lua
local magnetometer = require("magnetometer")
local sensor = magnetometer.new({ device = "magnetometer_sensor" })
local sample = sensor:read()
print(sample.magnetic.x, sample.magnetic.y, sample.magnetic.z)
sensor:close()
```

The named device must be supported by the board firmware. It is initialized if needed; opening an initialized sensor preserves its sampling settings. Closing the Lua handle leaves the board sensor active. Only one Lua handle can open a given board sensor at a time. The board sensor and its bus must remain available while Lua uses them.

## External sensor

```lua
local i2c = require("i2c")
local magnetometer = require("magnetometer")
local bus = i2c.new(1, 4, 5) -- Choose a controller and pins available on your board.
local sensor = magnetometer.new({ chip = "bmm150", bus = bus, address = 0x10 })
print(sensor:read().magnetic.x)
sensor:close()
bus:close()
```

An external sensor can also use an `i2c.borrow(port)` bus.

| Field | Description |
| --- | --- |
| `chip` | Required: `bmm150`, `bmm350`, or `qmc6309` |
| `bus` | Required Lua I2C bus handle |
| `address` | Optional 7-bit address: BMM150 `0x10`–`0x13` (default `0x10`), BMM350 `0x14`–`0x15` (default `0x14`), QMC6309 `0x7c` |
| `int_gpio` | Optional interrupt input GPIO; default `-1` (unused); no callbacks are installed |
| `sdo_gpio` | Optional address strap output GPIO for BMM150/BMM350; default `-1` (unused) |

External sensors use 400 kHz transfers. Their address and requested GPIOs must be unused. No alternate addresses are probed. For BMM150, the other address selection pin must already match the requested address. QMC6309 does not accept `sdo_gpio`.

Pass exactly one configuration table. Board mode accepts only `device`; external fields cannot be combined with it. Invalid options, unavailable devices, disabled chips, resource conflicts, and hardware failures raise Lua errors.

## Read and close

- `sensor:read()` returns `magnetic = {x, y, z}`, `raw_magnetic = {x, y, z}`, `status`, and `calibrated`. BMM350 also returns `temperature` in degrees Celsius; other chips omit it.
- `sensor:read_temperature()` returns BMM350 temperature in degrees Celsius; other chips raise an error.
- `sensor:read_int_status()` returns chip-specific status bits. Reading may clear hardware status bits.
- `sensor:chip_id()` returns the chip's identification value.
- `sensor:name()` returns the board device name or external chip name.
- `sensor:close()` releases the Lua handle and deletes an external sensor. Repeated successful closes are harmless.

BMM150 and BMM350 magnetic values are in microteslas; QMC6309 values are signed raw counts. `raw_magnetic` means before application calibration; factory compensation is already applied by the Bosch sensors. `magnetic` has the same units after calibration. No heading or tilt compensation is computed.

Reads are synchronous. QMC6309 waits for data readiness with a 100 ms polling deadline; an in-progress I2C transfer and scheduling can extend the elapsed time. BMM150/BMM350 read the current sample, so callers should allow startup time and choose a polling interval appropriate for the sensor. Do not access the same sensor concurrently through another driver.

Close an external sensor before closing its bus: a bus with an active sensor cannot close. Garbage collection also releases handles, but explicit close is recommended. Retry a failed close before discarding the handle. Read and calibration methods fail after close.

## Calibration

Calibration affects only the current Lua object and is not saved automatically. Applications can store the result of `calibration_get()` and restore it with `calibration_set()`.

- `calibration_reset()` clears calibration and starts a new collection.
- `calibration_add_sample([sample])` accepts an array `{x, y, z}` before application calibration, or reads a sample when omitted. Returns the collected sample count.
- `calibration_finish()` computes offset and per-axis scale, returning the calibration table. Requires at least 16 samples and a nonzero span on every axis. Rotate the sensor through different orientations while collecting samples.
- `calibration_get()` returns `hard_iron` (three-number array), `soft_iron` (3×3 array), `calibrated`, `collecting`, and `sample_count`.
- `calibration_set({hard_iron = {...}, soft_iron = {{...}, {...}, {...}}})` applies finite numeric coefficients and returns the calibration table.
- `calibration_clear()` clears calibration and collection state, returning the calibration table.

The correction is `soft_iron * (raw_magnetic - hard_iron)`. Without calibration, `magnetic` equals `raw_magnetic`. Keep saved coefficients associated with the individual sensor and its installation.
