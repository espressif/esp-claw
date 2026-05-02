# Lua I2C

This skill describes how to correctly use `i2c` when writing Lua scripts.
It is built on top of the `i2c_bus` component and supports scanning a bus
and talking to multiple devices on the same bus.

## How to call
- Import it with `local i2c = require("i2c")`
- Create a bus with `local bus = i2c.new(port, sda, scl [, freq_hz])`
  - `port`: I2C port number, usually `0` or `1`
  - `sda`, `scl`: GPIO numbers for SDA and SCL
  - `freq_hz`: optional clock frequency in Hz, default `400000`
- Scan the bus for devices with `local addrs = bus:scan()`
  - Returns a Lua array of 7-bit addresses that ACKed
- Attach a device with `local dev = bus:device(addr [, clk_speed])`
  - `addr`: 7-bit I2C address (0-127)
  - `clk_speed`: optional per-device override in Hz, `0` to inherit the bus speed
- Read/write on a device:
  - `dev:read_byte([mem_addr])` → integer (0-255)
  - `dev:read(len [, mem_addr])` → binary string of `len` bytes
  - `dev:write_byte(value [, mem_addr])`
  - `dev:write(data [, mem_addr])` where `data` is a string or a table of bytes
  - `dev:address()` → the 7-bit address
- Close handles when you're done with `dev:close()` and `bus:close()`.
  **Always close every device before closing its bus.** Handles are also
  cleaned up automatically on garbage collection, but explicit close is
  preferred so resources are released deterministically.

`mem_addr` is the 8-bit internal register/memory address inside the device.
Omit it (or pass `nil`) for devices that have no internal address. Any error
raises a Lua error, so wrap calls in `pcall` if you want to handle failures.

## Example: scan the bus
```lua
local i2c = require("i2c")

local bus = i2c.new(0, 21, 22, 400000)
for _, addr in ipairs(bus:scan()) do
    print(string.format("found device at 0x%02X", addr))
end
bus:close()
```

## Example: read a register from a device
```lua
local i2c = require("i2c")

local bus = i2c.new(0, 21, 22)
local dev = bus:device(0x68)             -- MPU6050-style address
local who_am_i = dev:read_byte(0x75)     -- read register 0x75
print(string.format("WHO_AM_I = 0x%02X", who_am_i))

dev:write_byte(0x00, 0x6B)               -- wake up (PWR_MGMT_1 = 0)
local raw = dev:read(14, 0x3B)           -- burst-read 14 bytes from 0x3B

dev:close()
bus:close()
```

## Example: write multiple bytes
```lua
-- pass a string
dev:write("\x01\x02\x03")

-- or a table of bytes (with optional leading register address)
dev:write({0xA0, 0x55, 0xAA}, 0x10)
```
