# Lua I2C

This module describes how to use `i2c` from Lua for I2C buses and 7-bit
addressed devices. It supports scanning a bus and talking to multiple devices
on the same bus.

## How to call
- Import it with `local i2c = require("i2c")`
- Create a bus with `local bus = i2c.new(port, sda, scl [, freq_hz])`
  - `port`: I2C port number, usually `0` or `1`
  - `sda`, `scl`: GPIO numbers for SDA and SCL
  - `freq_hz`: optional clock frequency in Hz, default `400000`
- Borrow a board-initialized bus with `local bus = i2c.borrow(0)`
  - The board must initialize the bus and keep it alive throughout Lua use; borrowing does not initialize or reconfigure its controller or pins. Do not borrow a bus created by `i2c.new()`; reuse its Lua handle instead
  - The borrowed Lua handle defaults to `400000` Hz for new devices; use `bus:device(addr, clk_speed)` to choose another device speed
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
  **Always close every device before closing its bus.** Closing a bus with live devices fails. Closing a borrowed handle leaves the board bus initialized.
  Unreachable handles are finalized by Lua GC; job exit closes remaining handles. Retry a failed explicit close before dropping the handle.

`i2c.new()` requires an unused controller and pins. On any bus, an address already used by another device cannot be opened again, including through `i2c.borrow()`. Use the device-specific module for an initialized board sensor.

`mem_addr` is the 8-bit internal register/memory address inside the device.
Omit it (or pass `nil`) for devices that have no internal address. Any error
raises a Lua error, so wrap calls in `pcall` if you want to handle failures.

## Reusable libraries
- `ssd1306`: Pure-Lua SSD1306 OLED helper for I2C panels. Use
  `local ssd1306 = require("ssd1306")` after opening an I2C device handle.
- `lib_si12t_touch`: Pure-Lua Si12T 12-channel capacitive touch helper. Use
  `local si12t_touch = require("lib_si12t_touch")`; it can reuse an existing
  I2C bus or create one from `port`, `sda`, and `scl` options.
- `lib_ina226`: Pure-Lua INA226 voltage / current / power monitor. Use
  `local ina226 = require("lib_ina226")`. `shunt_res` and
  `max_expected_current` are required, since a wrong shunt value yields
  readings that look plausible but are wrong by a constant factor.
- `lib_ltr553`: Pure-Lua LTR-553ALS ambient light and proximity sensor, the
  part fitted to M5Stack CoreS3. Use `local ltr553 = require("lib_ltr553")`;
  it reports lux plus the raw CH0/CH1 counts.
- `lib_py32_ioe`: Pure-Lua PY32 I2C IO expander (14 GPIO, 4 ADC, 4 PWM,
  addressable-LED RAM). Use `local py32_ioe = require("lib_py32_ioe")`.

All of these can reuse an existing bus handle. Passing `bus` is the right
choice when the board manager already owns the port — creating a second handle
for a port that is already up is safe, but the library must not close it.

Full API docs for these helpers are bundled with the libraries.

## Example: scan the bus
```lua
local i2c = require("i2c")

local bus = i2c.borrow(0)
for _, addr in ipairs(bus:scan()) do
    print(string.format("found device at 0x%02X", addr))
end
bus:close()
```

## Example: read a register from an external device

Connect a separate MPU6050-compatible sensor to free GPIO4/SDA and GPIO5/SCL on controller 1. Check that these pins and controller are available on your board. Use `imu.new({ device = "imu_sensor" })` for the built-in IMU instead.
```lua
local i2c = require("i2c")

local bus = i2c.new(1, 4, 5, 100000)
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
