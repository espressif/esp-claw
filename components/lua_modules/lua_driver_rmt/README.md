# Lua RMT

This module exposes the ESP-IDF RMT peripheral to Lua for raw symbol transmit
and receive. Use this driver for protocol libraries that need precise pulses,
such as IR, LED strips, pulse capture, or custom one-wire protocols.

## How to call

- Import it with `local rmt = require("rmt")`
- Create a TX channel with `local tx = rmt.tx({ gpio = 39 })`
- Create an RX channel with `local rx = rmt.rx({ gpio = 38 })`
- Send raw symbols with `tx:send(symbols [, timeout_ms])`
- Receive raw symbols with `local symbols, err = rx:receive(timeout_ms)`
- For loopback or externally triggered captures, arm RX first with `rx:start()`,
  then fetch the result with `local symbols, err = rx:read(timeout_ms)`
- Inspect configuration with `handle:info()`
- Close handles with `handle:close()`

## TX options

| Field               | Type    | Default | Meaning                         |
|---------------------|---------|---------|---------------------------------|
| `gpio`              | integer | required | Output GPIO                     |
| `resolution_hz`     | integer | `1000000` | RMT tick resolution             |
| `mem_block_symbols` | integer | `64`    | RMT memory block size           |
| `trans_queue_depth` | integer | `4`     | Pending transmit queue depth    |
| `carrier_hz`        | integer | unset   | Optional carrier frequency      |
| `carrier_duty`      | number  | `0.33`  | Carrier duty cycle, `0.0-1.0`   |

## RX options

| Field                 | Type    | Default | Meaning                         |
|-----------------------|---------|---------|---------------------------------|
| `gpio`                | integer | required | Input GPIO                      |
| `resolution_hz`       | integer | `1000000` | RMT tick resolution             |
| `mem_block_symbols`   | integer | `128`   | RMT memory block size           |
| `max_symbols`         | integer | `256`   | Maximum captured symbols        |
| `signal_range_min_ns` | integer | `1250`  | Shorter pulses are filtered     |
| `signal_range_max_ns` | integer | `32000000` | Longer idle ends a receive   |

## Symbol format

Each symbol is a table:

- `level0`: `0` or `1`
- `duration0`: RMT ticks
- `level1`: `0` or `1`
- `duration1`: RMT ticks

At the default `1000000` Hz resolution, one tick is one microsecond.

## Example

```lua
local rmt = require("rmt")

local tx = rmt.tx({ gpio = 39, carrier_hz = 38000 })
tx:send({
    { level0 = 1, duration0 = 9000, level1 = 0, duration1 = 4500 },
    { level0 = 1, duration0 = 560,  level1 = 0, duration1 = 560 },
})
tx:close()
```
