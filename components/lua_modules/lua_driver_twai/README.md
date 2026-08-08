# Lua TWAI (Classical CAN)

This module exposes the ESP-IDF TWAI controller to Lua for Classical CAN 2.0
frame transmission and reception. It owns the single TWAI controller until the
bus handle is closed.

## How to call

- Import it with `local twai = require("twai")`.
- Open and start the controller with `local bus = twai.new(config)`.
- Send a frame with `bus:send(id, data [, opts])`.
- Receive a frame with `local frame, err = bus:receive([timeout_ms])`.
- Inspect controller state and error counters with `bus:status()`.
- Clear pending received frames with `bus:clear_rx()`.
- Recover from bus-off with `bus:recover([timeout_ms])`.
- Release the controller and GPIOs with `bus:close()`.

Only one bus handle may exist at a time. Always call `close()` when a script is
finished; garbage collection also attempts cleanup.

## Configuration

```lua
local bus = twai.new({
    tx_gpio = 6,
    rx_gpio = 7,
    bitrate = 500000,
    mode = "normal",
    tx_queue_len = 8,
    rx_queue_len = 20,
})
```

`tx_gpio` and `rx_gpio` are required. Supported bit rates are 25 kbit/s,
50 kbit/s, 100 kbit/s, 125 kbit/s, 250 kbit/s, 500 kbit/s, 800 kbit/s, and
1 Mbit/s. Modes are `normal`, `no_ack`, and `listen_only`. Queue lengths use
the defaults shown above; transmit queue length may be zero only in
`listen_only` mode.

## Sending

`bus:send(id, data [, opts])` accepts an 11-bit identifier by default and data
as either a binary Lua string or a table of zero to eight byte integers.

Options:

- `timeout_ms`: time to wait for space in the transmit queue, default `0`.
- `extended`: use a 29-bit identifier, default `false`.
- `rtr`: send a remote transmission request, default `false`.
- `single_shot`: do not retry a failed transmission, default `false`.
- `self_reception`: also receive the transmitted frame, default `false`.

The call returns `true`, or `nil, "timeout"` / `nil, error` if the frame could
not be queued. Queue success does not prove another CAN node acknowledged the
frame; inspect `status()` for bus errors.

```lua
local ok, err = bus:send(0x123, {0, 0, 0, 0, 0, 0, 0, 1}, {
    timeout_ms = 20,
})
```

## Receiving

`bus:receive(timeout_ms)` returns a frame table or `nil, "timeout"`. The frame
contains:

- `id`, `dlc`, and a 1-based `data` byte table.
- `extended`, `rtr`, and `self_reception` booleans.

```lua
local frame, err = bus:receive(100)
if frame then
    print(string.format("RX id=0x%X dlc=%d", frame.id, frame.dlc))
end
```

## Status and recovery

`bus:status()` returns `state` (`running`, `stopped`, `bus_off`, or
`recovering`), configured pins/bitrate/mode, queue counts, error counters, and
accumulated transmit/receive error statistics.

`bus:recover(timeout_ms)` initiates ISO 11898-1 bus-off recovery, waits for it
to complete, and restarts the controller. It returns `true` or
`nil, "timeout"`. Recovery cannot succeed while the physical bus is held
dominant or is otherwise invalid.

## Hardware constraints

- ESP32-S3 TWAI supports Classical CAN only, not CAN FD frames. A CAN-FD-capable
  transceiver may be used for Classical CAN at a supported bit rate.
- The ESP32 connects to a CAN transceiver's logic TX/RX pins, not directly to
  CANH/CANL.
- Use a common logic-side ground and the voltage required by the transceiver
  module. Verify that its RX output is safe for 3.3 V ESP32 GPIO.
- Terminate CANH/CANL with 120 ohms at each physical end of the bus. Normal mode
  needs another active CAN node to acknowledge frames.
