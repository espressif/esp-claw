# Lua UART

Use `uart` for asynchronous serial communication from Lua. It is suitable for UART-to-RS485 transceivers that handle bus direction automatically.

## Create a UART

```lua
local uart = require("uart")
local serial = uart.new(1, 17, 18, 115200)
```

The arguments are UART port, TX GPIO, RX GPIO, and baud rate. The UART uses 8 data bits, no parity, one stop bit, and no hardware flow control (8N1).

## Methods

- `serial:write(data)` queues a Lua string and returns the number of bytes.
- `serial:available()` returns the number of buffered RX bytes.
- `serial:read(max_length, timeout_ms)` returns up to `max_length` bytes, or `nil` when no byte is received before the timeout.
- `serial:read_line(max_length, timeout_ms)` reads through `\n`, the size limit, or the timeout. It returns `nil` if no byte is received.
- `serial:flush_input()` discards buffered RX data.
- `serial:wait_tx_done(timeout_ms)` waits for queued output to leave the UART.
- `serial:close()` releases the UART driver. Always close the handle when a script stops.

Read operations accept at most 4096 bytes. Timeout values are milliseconds. Only one open handle may own a UART port at a time.

## RS485 example

```lua
local uart = require("uart")
local serial = uart.new(1, 17, 18, 115200)

serial:flush_input()
serial:write("hello\n")
serial:wait_tx_done(1000)

if serial:available() > 0 then
    local line = serial:read_line(128, 200)
    if line then
        print(line)
    end
end

serial:close()
```

For an RS485 transceiver with a DE/RE direction pin, control that pin with the `gpio` module before and after transmission. The built-in RS485 scripts assume an automatic-direction transceiver such as the TD501D485H-A.
