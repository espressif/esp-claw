# lib_si12t_touch.lua

Reusable Lua driver for the Si12T 12-channel capacitive touch IC (TS1..TS12). It uses the builtin `i2c` module and exports `require("lib_si12t_touch")`.

## When to use

Use this library when a script needs to read the touched/untouched state of a Si12T's 12 capacitive channels. The IC is self-contained on a single I2C address (`0x68` when `ID_SEL=GND`, `0x78` when `ID_SEL=VDD`).

## Loading

```lua
local si12t_touch = require("lib_si12t_touch")
```

The script must also have access to the `i2c` module. You can either pass an existing I2C bus handle or let the library create one from GPIO options.

## Constructor

```lua
local touch = si12t_touch.new(opts)
```

`opts` is a table:

- `bus`: existing I2C bus userdata. Recommended when the script already owns a bus.
- `port`: I2C port number. Required if `bus` is not provided.
- `sda`: SDA GPIO. Required if `bus` is not provided.
- `scl`: SCL GPIO. Required if `bus` is not provided.
- `freq_hz`: I2C frequency in Hz. Defaults to `100000`.
- `frequency`: alias of `freq_hz`.
- `addr`: 7-bit I2C address. Must be `0x68` or `0x78`. Defaults to `0x78`.
- `threshold`: sensitivity 0..7 (lower = more sensitive). Defaults to `3`.
- `channels`: which channels to enable. Defaults to all 12. Accepts:
    - `nil` or `"all"` — enable all 12 channels.
    - integer bitmask, TS1=bit0..TS12=bit11 (e.g. `0x007` for TS1/TS2/TS3).
    - array of 1-based channel numbers, e.g. `{1, 2, 3}`.
    - comma-separated string, e.g. `"1,2,3"`.

If `bus` is omitted, the library creates and owns the I2C bus and will close it from `touch:close()` when `opts.close_bus = true`.

## Methods

- `touch:address()`: returns the configured 7-bit I2C address.
- `touch:threshold()`: returns the current threshold (0..7).
- `touch:channel_mask()`: returns the current 12-bit enabled-channel bitmask.
- `touch:read()`: returns a 12-bit bitmask of currently-touched channels (TS1=bit0). Disabled channels are masked out.
- `touch:read_channels()`: returns `{ [1]=bool, ..., [12]=bool }`.
- `touch:set_threshold(value)`: re-arm the chip with a new sensitivity (0..7).
- `touch:set_channels(spec)`: re-arm the chip with a new enabled-channel set. `spec` accepts the same forms as the `channels` option.
- `touch:close()`: closes the I2C device and, if owned by the touch handle, the I2C bus.

## Module constants

- `si12t_touch.channels()`: returns `12`.
- `si12t_touch.channel_mask_all()`: returns `0xFFF`.

## Example

```lua
local si12t_touch = require("lib_si12t_touch")
local i2c = require("i2c")

local bus = i2c.new(0, 39, 40, 100000)
local touch = si12t_touch.new({
    bus = bus,
    -- addr = 0x78,           -- default
    -- threshold = 3,         -- default
    channels = {1, 2, 3},     -- only TS1/TS2/TS3
})

local mask = touch:read()
print(string.format("status=0x%03X", mask))

touch:close()
bus:close()
```
