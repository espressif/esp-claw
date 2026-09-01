# Lua Delay

This module describes how to correctly use delay when writing Lua scripts.

## How to call
- Import it with `local delay = require("delay")`
- Call `delay.delay_ms(ms)` to sleep for a number of milliseconds
- Call `delay.delay_us(us)` for short microsecond delays
- Call `delay.periodic(period_ms)` to create a periodic waiter backed by `xTaskDelayUntil()`
- **`ms` must be an integer**
- **`us` must be an integer**
- Negative `ms` and `us` values are accepted but clamped to `0`
- `period_ms` must be a positive integer and at least one FreeRTOS tick
- `delay_us(us)` is a busy-wait intended for short hardware timing only
- `delay_us(us)` accepts `0..1000000`; use `delay_ms(ms)` for longer waits
- `periodic:wait()` returns `true` when the task waited, or `false` when the deadline was already missed
- `periodic:reset()` starts the schedule again from the current tick

## Example
```lua
local delay = require("delay")
delay.delay_ms(500)
delay.delay_us(200)

local ticker = delay.periodic(30)
for _ = 1, 10 do
    -- periodic work
    ticker:wait()
end

ticker:reset()
```
