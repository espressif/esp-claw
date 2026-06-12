# ir_driver

`ir_driver` is a Lua IR helper built on top of the low-level `rmt` driver.
Use it when a script needs IR learn/replay behavior without depending on the
native `ir` C module.

Load it with:

```lua
local ir_driver = require("ir_driver")
```

## Constructor

```lua
local dev = ir_driver.new({
    tx_gpio = 39,
    rx_gpio = 38,
    ctrl_gpio = 44,
    carrier_hz = 38000,
})
```

Options:
- `tx_gpio`: IR transmitter GPIO.
- `rx_gpio`: IR receiver GPIO.
- `ctrl_gpio`: optional enable/power GPIO.
- `ctrl_active_level`: active level for `ctrl_gpio`, default `0`.
- `carrier_hz`: TX carrier frequency, default `38000`.
- `tx_resolution_hz`: RMT TX resolution, default `1000000`.
- `rx_resolution_hz`: RMT RX resolution, default `1000000`.
- `rx_max_symbols`: maximum captured symbols, default `256`.
- `signal_range_min_ns`: RX glitch filter threshold, default `1250`.
- `signal_range_max_ns`: RX idle-end threshold, default `32000000`.
- `rx_invert`: invert received levels, default `true` for active-low IR receivers.

At least one of `tx_gpio` or `rx_gpio` is required.

## Raw Symbols

Symbols use the same shape as the `rmt` driver:

```lua
{
    { level0 = 1, duration0 = 9000, level1 = 0, duration1 = 4500 },
    { level0 = 1, duration0 = 560,  level1 = 0, duration1 = 560 },
}
```

At the default 1 MHz resolution, durations are microseconds.

## Methods

- `dev:send_raw(symbols [, timeout_ms])`
- `dev:send_nec(address, command [, timeout_ms])`
- `dev:receive(timeout_ms)` -> `symbols` or `nil, err`
- `dev:start_receive()` then `dev:read_receive(timeout_ms)` for loopback or externally triggered captures
- `dev:receive_nec(timeout_ms [, tolerance])` -> decoded table or `nil, err [, symbols]`
- `dev:info()`
- `dev:name()`
- `dev:close()`

`ir_driver.build_nec(address, command)` returns NEC symbols without opening hardware.
`ir_driver.decode_nec(symbols [, tolerance])` decodes NEC symbols and returns:

```lua
{
    address = 0x00FF,
    command = 0x10EF,
    raw = 0x10EF00FF,
}
```

## Example

```lua
local ir_driver = require("ir_driver")

local dev = ir_driver.new({
    tx_gpio = 39,
    rx_gpio = 38,
    ctrl_gpio = 44,
})

local symbols, err = dev:receive(5000)
if symbols then
    dev:send_raw(symbols)
else
    print("receive failed: " .. tostring(err))
    dev:send_nec(0x00FF, 0x10EF)
end

dev:close()
```
