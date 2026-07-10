# Lua DFRobot SCI

This module provides access to the DFRobot RP2040 SCI Acquisition Module
(DFR0999) from Lua. It talks to the SCI board over I2C and returns sensor data
in the DFRobot SCI string format.

## Hardware
- Connect the ESP board I2C SDA/SCL pins to the SCI board host I2C port.
- On DFRobot K10, the board-managed I2C bus is usually `port=1`, `SDA=47`,
  `SCL=48`.
- The SCI default 7-bit I2C address is `0x21`; common addresses are `0x21`,
  `0x22`, and `0x23`.
- Use `100000` Hz I2C clock unless the module has been verified at another
  speed.

## How to call
- Import it with `local sci = require("sci")`.
- Open the module with
  `local dev = sci.new(port, sda_gpio, scl_gpio [, addr [, freq_hz]])`.
- Close the handle with `dev:close()` when done.

## Common reads
- `dev:get_version()` returns `{ raw = 0x0105, text = "V1.0.5" }`.
- `dev:get_information([port_mask [, timestamp]])` returns readings such as
  `"SEN0334: Temp_Air:28.65 C,Humi_Air:30.12 %RH"`.
- `dev:get_sku([port_mask])` returns connected sensor SKUs.
- `dev:get_keys([port_mask])`, `dev:get_values([port_mask])`, and
  `dev:get_units([port_mask])` return comma-separated names, values, and units.
- `dev:get_value(key [, port_mask [, sku]])` reads values for one data name.
- `dev:get_unit(key [, port_mask [, sku]])` reads units for one data name.
- `dev:get_timestamp()` returns the SCI data refresh timestamp string.

Port masks:
- `sci.PORT1` for the A/D port
- `sci.PORT2` for I2C/UART port 2
- `sci.PORT3` for I2C/UART port 3
- `sci.ALL` for all ports

## Configuration
- `dev:set_port(1, sku)`, `dev:set_port(2, sku)`, and `dev:set_port(3, sku)`
  configure ports. Use `"NULL"` to clear a port. Use `"Analog"` for raw analog
  voltage on port 1.
- `dev:get_port(1)`, `dev:get_port(2)`, and `dev:get_port(3)` return
  `{ mode = 0|1, mode_text = "...", sku = "..." }`.
- `dev:set_refresh_rate(rate)` accepts `sci.REFRESH_MS`, `sci.REFRESH_1S`,
  `sci.REFRESH_3S`, `sci.REFRESH_5S`, `sci.REFRESH_10S`,
  `sci.REFRESH_30S`, `sci.REFRESH_1MIN`, `sci.REFRESH_5MIN`, or
  `sci.REFRESH_10MIN`.
- `dev:get_refresh_rate()` returns `{ rate = enum_value, ms = milliseconds }`.
- `dev:enable_record()` / `dev:disable_record()` control SCI CSV recording.
- `dev:oled_on()` / `dev:oled_off()` control the SCI onboard display.
- `dev:get_supported_skus(kind)` where `kind` is `"analog"`, `"digital"`,
  `"i2c"`, or `"uart"` returns the supported SKU list.

Methods that configure the SCI return the DFRobot SCI error code. `0` means
success.

## Example
```lua
local sci = require("sci")

local dev = sci.new(1, 47, 48, 0x21, 100000)
dev:set_refresh_rate(sci.REFRESH_1S)

local version = dev:get_version()
print("SCI firmware: " .. version.text)
print("SKUs: " .. dev:get_sku(sci.ALL))
print("Readings: " .. dev:get_information(sci.ALL, true))

dev:close()
```

If the SCI board is not found, run `builtin/sci_probe.lua`. It tries the K10
I2C bus plus common external I2C pin pairs and SCI addresses `0x21`, `0x22`,
and `0x23`, then prints the first working version/SKU/information response.
