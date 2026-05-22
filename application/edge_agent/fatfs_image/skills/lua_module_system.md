# Lua System

This skill describes how to correctly use system when writing Lua scripts.

## How to call
- Import it with `local system = require("system")`
- This module does not require manual initialization after `require`

## API

### `system.time()`
- Inputs: none
- Output: `number`
- Returns the current Unix timestamp in seconds
- Raises an error if the system clock is not set

### `system.date(format)`
- Inputs:
  - `format`: optional `string`, defaults to `"%Y-%m-%d %H:%M:%S"`
- Output: `string`
- Returns the formatted local time string using `strftime`-style placeholders
- Raises an error if the format is too long or produces an empty string

### `system.millis()`
- Inputs: none
- Output: `number`
- Returns the device uptime in milliseconds

### `system.uptime()`
- Inputs: none
- Output: `integer`
- Returns the device uptime in seconds

### `system.ip()`
- Inputs: none
- Output: `string | nil`
- Returns the current Wi-Fi STA IPv4 address
- Returns `nil` when the device is not connected or no IP address is assigned

### `system.info()`
- Inputs: none
- Output: `table`
- Returns a table with these possible fields:
  - `uptime_s`: `integer`, uptime in seconds
  - `time`: `number`, current Unix timestamp in seconds
  - `date`: `string`, formatted local time as `%Y-%m-%d %H:%M:%S`
  - `sram_free`: `integer`, free internal SRAM bytes
  - `sram_total`: `integer`, total internal SRAM bytes
  - `sram_largest`: `integer`, largest free internal SRAM block in bytes
  - `psram_free`: `integer`, free PSRAM bytes, present only when PSRAM exists
  - `psram_total`: `integer`, total PSRAM bytes, present only when PSRAM exists
  - `wifi_rssi`: `integer`, connected AP RSSI, present only when Wi-Fi STA is connected
  - `wifi_ssid`: `string`, connected AP SSID, present only when available

## Example
```lua
local system = require("system")

print(system.time())
print(system.date("%Y-%m-%d %H:%M:%S"))
print(system.millis())
print(system.uptime())
print(system.ip())

local info = system.info()
print(info.sram_free)
```
