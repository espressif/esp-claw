# lib_bq27220.lua

Reusable Lua driver for the Texas Instruments BQ27220 battery fuel gauge. It uses the builtin `i2c` module and exports `require("lib_bq27220")`.

## When to use

Use this library when a script needs battery state of charge, voltage, or current from a BQ27220 connected over I2C.

## Loading

```lua
local bq27220 = require("lib_bq27220")
```

The script must also have access to the `i2c` module. You can either pass an existing I2C bus handle or let the library create one from GPIO options.

## Constructor

```lua
local gauge = bq27220.new(opts)
```

`opts` is a table:

- `bus`: existing I2C bus userdata. This is recommended when the script already owns a bus.
- `port`: I2C port number. Required if `bus` is not provided.
- `sda`: SDA GPIO. Required if `bus` is not provided.
- `scl`: SCL GPIO. Required if `bus` is not provided.
- `freq_hz`: I2C frequency in Hz. Defaults to `400000`.
- `frequency`: alias of `freq_hz`.
- `addr`: BQ27220 7-bit address. Defaults to `0x55`.

If `bus` is omitted, the library creates and owns the I2C bus and will close it from `gauge:close()`.

## Methods

- `gauge:address()`: returns the configured 7-bit I2C address.
- `gauge:read_voltage_mv()`: returns voltage in millivolts.
- `gauge:read_current_ma()`: returns signed current in milliamps.
- `gauge:read_soc()`: returns state of charge in percent.
- `gauge:read()`: returns `{ voltage_mv = number, current_ma = number, soc = number }`.
- `gauge:close()`: closes the I2C device and, if owned by the gauge, the I2C bus.

## Example

```lua
local bq27220 = require("lib_bq27220")
local i2c = require("i2c")

local bus = i2c.new(0, 14, 13, 400000)
local gauge = bq27220.new({
    bus = bus,
    addr = 0x55,
})

local sample = gauge:read()
print(sample.soc, sample.voltage_mv, sample.current_ma)

gauge:close()
bus:close()
```
