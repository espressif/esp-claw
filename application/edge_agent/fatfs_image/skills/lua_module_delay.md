# Lua Delay

This skill describes how to correctly use delay when writing Lua scripts.

## How to call
- Import it with `local delay = require("delay")`
- Call `delay.delay_ms(ms)` to sleep for a number of milliseconds
- Call `delay.delay_us(us)` for short microsecond delays
- **`ms` must be an integer**
- **`us` must be an integer**
- Negative values are accepted but clamped to `0`
- `delay_us(us)` is a busy-wait intended for short hardware timing only
- `delay_us(us)` accepts `0..1000000`; use `delay_ms(ms)` for longer waits

## Example
```lua
local delay = require("delay")
delay.delay_ms(500)
delay.delay_us(200)
```
