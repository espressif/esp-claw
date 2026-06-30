# Lua PCNT

This module is PCNT/ENCODER driver for pulse counting from Lua.

## How to call
- Import it with `local pcnt = require("pcnt")`
- Create a unit with `local unit = pcnt.new({ edge_gpio = 4 })`
- Start counting with `unit:start()`
- Read the current count with `unit:get_count()`
- Clear the counter with `unit:clear()`
- Stop counting with `unit:stop()`
- Release resources with `unit:close()`

## Config table
- `low_limit`: optional, defaults to `-32768`
- `high_limit`: optional, defaults to `32767`
- `accum_count`: optional, defaults to `false`
- `glitch_ns`: optional glitch filter width in nanoseconds
- `edge_gpio`: optional edge input GPIO for the first channel
- `level_gpio`: optional level input GPIO for the first channel
- `pos_edge`: optional, one of `"hold"`, `"increase"`, `"decrease"`, defaults to `"increase"`
- `neg_edge`: optional, one of `"hold"`, `"increase"`, `"decrease"`, defaults to `"hold"`
- `high_level`: optional, one of `"keep"`, `"inverse"`, `"hold"`, defaults to `"keep"`
- `low_level`: optional, one of `"keep"`, `"inverse"`, `"hold"`, defaults to `"keep"`
- `invert_edge`: optional, defaults to `false`
- `invert_level`: optional, defaults to `false`

Additional channels can be added before `start()` with `unit:add_channel(opts)`,
using the same channel fields.

## Example
```lua
local pcnt = require("pcnt")
local delay = require("delay")

local unit = pcnt.new({
    edge_gpio = 4,
    glitch_ns = 1000,
})

unit:start()
delay.delay_ms(1000)
print("count", unit:get_count())
unit:clear()
unit:stop()
unit:close()
```

## Rotary encoder example

For an EC11-style quadrature encoder, wire channel A and channel B to two
PCNT-capable GPIOs. The channel action mapping below follows ESP-IDF's
`examples/peripherals/pcnt/rotary_encoder` example:

```lua
local pcnt = require("pcnt")
local delay = require("delay")

local gpio_a = 0
local gpio_b = 2

local encoder = pcnt.new({
    low_limit = -100,
    high_limit = 100,
    accum_count = true,
    glitch_ns = 1000,
    edge_gpio = gpio_a,
    level_gpio = gpio_b,
    pos_edge = "decrease",
    neg_edge = "increase",
    high_level = "keep",
    low_level = "inverse",
})

encoder:add_channel({
    edge_gpio = gpio_b,
    level_gpio = gpio_a,
    pos_edge = "increase",
    neg_edge = "decrease",
    high_level = "keep",
    low_level = "inverse",
})

encoder:clear()
encoder:start()

while true do
    print("encoder count", encoder:get_count())
    delay.delay_ms(1000)
end
```

The same example is available as `test/rotary_encoder.lua`. Adjust `gpio_a`
and `gpio_b` for your board before running it.
