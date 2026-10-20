# Lua DFRobot SCI

`require("sci")` provides DFRobot SCI sensor-board commands over an existing `i2c` bus. Enable `CONFIG_APP_CLAW_LUA_MODULE_SCI` in the firmware.

## Quick start

```lua
local i2c = require("i2c")
local sci = require("sci")
local bus = i2c.new(0, 8, 9, 100000) -- Choose pins for your board.
local device = sci.new({bus = bus, address = sci.DEFAULT_ADDR})
print(device:get_version().text)
print(device:get_values())
device:close()
bus:close()
```

## Create and close

`sci.new({bus = bus, address = 0x21, freq_hz = 100000, timeout_ms = 2000})` returns a device. `bus` must be an open bus from `i2c.new()` or `i2c.borrow()`. The address is a 7-bit value (0–127); frequency must be 1–100000 Hz and timeout must be 1–2147483647 ms. Construction reserves the address but does not verify that a board is connected. Unknown options and extra constructor arguments raise errors.

Close the SCI device before closing its bus. `device:close()` is safe to repeat; if it raises an error, retry it to finish cleanup. Garbage collection also attempts cleanup. Operations on a closed device raise an error.

`device:set_timeout(timeout_ms)` changes the I2C transaction and response-poll timeout. A whole command can take longer because it includes multiple transactions and protocol recovery.

## Commands

Queries raise a Lua error on communication or protocol failure. Unless specified otherwise, configuration commands return a numeric SCI result: `0` means success, and a nonzero result means failure.

| Method | Result or arguments |
| --- | --- |
| `get_version()` | `{raw = integer, text = "Vx.y.z"}` |
| `get_address()` | Current address reported by the board |
| `set_address(address)` | Returns `0` on success; raises an error on failure. See below. |
| `set_port(port, sku)` | Port number 1, 2, or 3; SKU up to 7 bytes |
| `get_port(port)` | `{mode, mode_text, sku}`; port 1 is analog/digital, ports 2–3 are I2C/UART |
| `set_refresh_rate(rate)` | Rate 0–8, or a refresh constant below |
| `get_refresh_rate()` | `{rate, ms}`; rate 0 reports `ms = 0` |
| `enable_record()`, `disable_record()` | Enable or disable recording |
| `oled_on()`, `oled_off()` | Control the board display |
| `get_information(mask, formatted)` | Response string; mask defaults to `sci.ALL`, formatted is a boolean |
| `get_sku(mask)`, `get_keys(mask)`, `get_values(mask)`, `get_units(mask)` | Response strings; mask defaults to `sci.ALL` |
| `get_value(key, mask, sku)`, `get_unit(key, mask, sku)` | Response string; optional mask/SKU restrict the lookup. Total request payload must not exceed 1024 bytes. |
| `get_supported_skus(kind)` | Response string; kind is `"analog"`, `"digital"`, `"i2c"`, or `"uart"` |
| `get_timestamp()` | Timestamp response string |
| `get_rtc()` | `{second, minute, hour, day, week, month, year}` |
| `set_rtc(fields)` | Requires all seven RTC fields. Seconds/minutes: 0–59; hour: 0–23; day: 1–31; week: 0–255 (use the board's weekday convention); month: 1–12; year: 0–65535 |
| `reset(command)` | Resets the selected protocol command, defaults to `0x14`; accepts 0–`0x14`, raises an error on write failure |

`set_address()` reserves the target address before changing the board. If the target cannot be reserved, the device remains usable at its original address. If changing the address or subsequent cleanup fails, the object becomes unusable: call `close()` and reopen using the board's actual address. A communication failure can leave the board address uncertain.

Port masks can be combined with Lua's bitwise OR: `sci.PORT1 | sci.PORT3`. They differ from the port numbers accepted by `set_port()` and `get_port()`.

## Constants

- `DEFAULT_ADDR = 0x21`
- `PORT1 = 1`, `PORT2 = 2`, `PORT3 = 4`, `ALL = 7`
- `REFRESH_MS = 0`
- `REFRESH_1S = 1`, `REFRESH_3S = 2`, `REFRESH_5S = 3`, `REFRESH_10S = 4`, `REFRESH_30S = 5`
- `REFRESH_1MIN = 6`, `REFRESH_5MIN = 7`, `REFRESH_10MIN = 8`
