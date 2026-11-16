# Lua UART

This module describes how to open a UART port and read/write bytes or text lines from Lua. Scripts call `read` / `read_line` with a timeout and get back whatever is available.

## How to call
- Import it with `local uart = require("uart")`
- Open a port with `local u = uart.new(port, tx, rx, baud [, opts])`
  - `port`: UART port number. **Start from `1`** by default. Avoid the port used by the
    console, commonly port `0`, because script traffic would interfere with logs.
    Each port has a single owner; a second `uart.new()` for the same
    port raises a Lua error.
  - `tx`, `rx`: Explicit GPIO numbers for TX and RX pins; `-1` is not accepted
  - `baud`: baud rate, e.g. `9600`, `115200`
  - `opts` (optional table, omit entirely for the common **8N1** case):
    - `data_bits`: `5`–`8`, default `8`
    - `parity`: `"none"` / `"even"` / `"odd"`, default `"none"`
    - `stop_bits`: `1` or `2`, default `1`
  - Writes block until sent. Receive capacity is 1 KiB; flow control is disabled.
- `u:read(len [, timeout_ms])` → string of up to `len` bytes. Timeout in
  milliseconds, default `0` (non-blocking — returns immediately with
  whatever is buffered, possibly empty). The returned string may be
  shorter than `len` if the timeout fires first.
- `u:read_line([max_len, timeout_ms])` → string ending with `\n` (or
  truncated at `max_len` / timeout). Default `max_len` is `1024`. The
  trailing `\n` is kept; strip with `line:gsub("[\r\n]+$", "")` if needed.
- `u:write(data)` → number of bytes sent. `data` is a string or a table
  of byte integers `0..255`.
- `u:available()` → number of bytes currently sitting in the RX buffer.
- `u:flush_input()` → discard all buffered RX data.
- `u:close()` when you're done. Job shutdown also closes retained handles; explicit close releases resources immediately.

## Example: AT-command style request/response
```lua
local uart = require("uart")
local delay = require("delay")

local u = uart.new(1, 17, 18, 115200)
u:flush_input()
u:write("AT\r\n")

delay.delay_ms(50)
local reply = u:read_line(128, 500)   -- up to 500 ms for the response
print("reply:", reply)

u:close()
```

## Example: binary polling loop
```lua
local uart = require("uart")
local delay = require("delay")

local u = uart.new(1, 17, 18, 9600)
for _ = 1, 10 do
    if u:available() > 0 then
        local chunk = u:read(64)      -- non-blocking
        -- process `chunk` (string; may be 1..64 bytes)
    end
    delay.delay_ms(20)
end
u:close()
```

## Example: non-8N1 frame format
Most serial devices are 8N1, but some legacy / industrial protocols
(e.g. Modbus ASCII, some meters) use 7 data bits with parity. Pass an
`opts` table to override; any field left out keeps its 8N1 default.
```lua
local u = uart.new(1, 17, 18, 9600, {
    data_bits = 7,
    parity    = "even",
    stop_bits = 1,
})
```

## Reusable libraries
- `lib_scs_servo`: Pure-Lua driver for Feetech SCS serial-bus servos (SCSCL
  memory table). Use `local scs_servo = require("lib_scs_servo")`; it can reuse
  an existing UART handle or open one from `port`, `tx`, `rx` and `baud`.
  Handles framing, checksums and the sign-magnitude feedback fields, and speaks
  raw servo counts rather than degrees. On M5Stack StackChan the bus is port
  `1`, `tx = 6`, `rx = 7` at 1 Mbps.

Full API docs for these helpers are bundled with the libraries.

## Notes
- All reads are **polling with a timeout**. There is no callback /
  interrupt interface in this module. For high-rate data, poll often
  enough to drain the 1 KiB RX buffer before it overruns.
- Closing a port releases it; the same `port` number can then be reopened with different settings.

Use free GPIOs suitable for the connected hardware. Occupied or unavailable resources raise an error. If `close()` fails, retry it before reusing the resources.
