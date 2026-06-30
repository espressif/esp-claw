# Lua IR

This module describes how to use `ir` from Lua for IR transmit and receive.
When a request mentions `ir`, `infrared`, `remote control`, `learn remote`, or `send NEC`, use this module by default.

## How to call

- Import it with `local ir = require("ir")`
- Open the board IR device with one of:
  - `local dev = ir.new()` - use board defaults from the device named `ir_blaster`
  - `local dev = ir.new("ir_blaster")` - choose a board device name explicitly
  - `local dev = ir.new({ tx_gpio = 39, rx_gpio = 38, ctrl_gpio = 44 })` - provide options directly
  - `local dev = ir.new("ir_blaster", { carrier_hz = 38000 })` - board defaults plus per-field overrides
- Call `dev:send_nec(address, command)` to transmit a 32-bit NEC frame
- Call `dev:send_raw(symbols)` to transmit raw RMT symbols
- Call `dev:receive(timeout_ms)` to learn one IR frame
- Call `dev:info()` to inspect the resolved configuration
- Call `dev:name()` to get the device name
- Call `dev:close()` when needed

## Options table

All fields are optional when the board declares an `ir_blaster` device in `board_devices.yaml`.
On a board that does not declare the device, at least one of `tx_gpio` or `rx_gpio` must be provided.

| Field               | Type    | Meaning                                      |
|---------------------|---------|----------------------------------------------|
| `device`            | string  | Board device name to read defaults from      |
| `tx_gpio`           | integer | GPIO used by the IR transmitter              |
| `rx_gpio`           | integer | GPIO used by the IR receiver                 |
| `ctrl_gpio`         | integer | Optional GPIO that powers/enables IR circuit |
| `ctrl_active_level` | integer | Active level for `ctrl_gpio`, default `0`    |
| `carrier_hz`        | integer | IR carrier frequency, default `38000`        |
| `tx_resolution_hz`  | integer | RMT TX resolution                            |
| `rx_resolution_hz`  | integer | RMT RX resolution                            |
| `rx_max_symbols`    | integer | Maximum raw symbols to capture per frame     |

## Data format

`dev:receive(timeout_ms)` returns a symbol array, or `nil, "timeout"` if no valid frame is captured before the timeout.
Each symbol is a table:

- `symbol.level0`
- `symbol.duration0`
- `symbol.level1`
- `symbol.duration1`

`dev:info()` returns:

- `info.name`
- `info.tx_gpio`
- `info.rx_gpio`
- `info.ctrl_gpio`
- `info.carrier_hz`
- `info.tx_resolution_hz`
- `info.rx_resolution_hz`

## Example

```lua
local ir = require("ir")

local dev = ir.new("ir_blaster")
local info = dev:info()
print(info.name, info.tx_gpio, info.rx_gpio, info.carrier_hz)

local symbols, err = dev:receive(5000)
if symbols then
    dev:send_raw(symbols)
else
    print("receive failed: " .. tostring(err))
    dev:send_nec(0x00FF, 0x10EF)
end

dev:close()
```
