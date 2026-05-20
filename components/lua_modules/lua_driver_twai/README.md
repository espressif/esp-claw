# Lua TWAI (CAN bus)

This module exposes the ESP32 TWAI (Two-Wire Automotive Interface / CAN bus) peripheral
to Lua scripts. It wraps the ESP-IDF v6 `esp_driver_twai` API and uses a FreeRTOS queue
to buffer received frames so scripts can poll with a timeout.

## How to call
- Import it with `local twai = require("twai")`
- Create a node with `local can = twai.new(tx_gpio, rx_gpio, bitrate [, opts])`
  - `tx_gpio`, `rx_gpio`: GPIO numbers for TWAI TX and RX lines
  - `bitrate`: bus speed in bits/s, e.g. `125000`, `250000`, `500000`, `1000000`
  - `opts` (optional table):
    - `listen_only`: `true` to monitor without transmitting (default `false`)
    - `loopback`: `true` for self-test mode (default `false`)
- `can:enable()` — start the node (must call before send/receive)
- `can:disable()` — stop the node (node handle is still valid)
- `can:send(id, data [, is_extended [, is_rtr]])` — transmit a frame
  - `id`: 11-bit (standard) or 29-bit (extended) arbitration ID
  - `data`: string of up to 8 bytes, e.g. `"\x01\x02\x03"`
  - `is_extended`: `true` for 29-bit ID (default `false`)
  - `is_rtr`: `true` for Remote Frame (default `false`)
- `can:receive([timeout_ms])` → table or `nil`
  - `timeout_ms`: how long to wait for a frame (`0` = non-blocking, `-1` = forever)
  - Returns `{id=..., data="...", extended=bool, rtr=bool}` or `nil` on timeout
- `can:status()` → `{state, tx_errors, rx_errors, tx_queue_remaining, bus_errors}`
  - `state`: `"active"`, `"warning"`, `"passive"`, or `"bus_off"`
- `can:recover()` — initiate bus-off recovery
- `can:close()` — disable and delete the node, free resources

## Example: send and receive
```lua
local twai = require("twai")

local can = twai.new(10, 11, 500000)
can:enable()

-- Send a standard frame (11-bit ID)
can:send(0x123, "\x01\x02\x03\x04")

-- Send an extended frame (29-bit ID)
can:send(0x1ABCDEF, "\xFF\xFE", true)

-- Poll for an incoming frame (100 ms timeout)
local frame = can:receive(100)
if frame then
    print(string.format("id=0x%X ext=%s rtr=%s data=%q",
        frame.id, tostring(frame.extended), tostring(frame.rtr), frame.data))
end

local st = can:status()
print("state:", st.state, "tx_err:", st.tx_errors, "rx_err:", st.rx_errors)

can:close()
```

## Example: continuous listener
```lua
local twai = require("twai")
local can = twai.new(10, 11, 250000, {listen_only = true})
can:enable()

while true do
    local f = can:receive(500)
    if f then
        print(string.format("[0x%03X] %s", f.id, f.data:gsub(".", function(c)
            return string.format("%02X ", c:byte())
        end)))
    end
end
```
