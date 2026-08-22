# Lua PCNT

This module provides pulse counting from Lua.

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

Additional channels can be added before `start()` with `unit:add_channel(opts)`, using the same channel fields.

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

## Return values
- `pcnt.new(opts)` returns a unit object.
- `unit:add_channel(opts)`, `unit:start()`, `unit:stop()`, `unit:clear()`, and `unit:close()` return no values on success and raise a Lua error on failure.
- `unit:get_count()` returns the current count as an integer.
