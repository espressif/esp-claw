# Lua MCPWM

This skill describes how to use `mcpwm` from Lua for generic PWM output.

## How to call
- Import it with `local mcpwm = require("mcpwm")`
- Create a PWM handle with `local pwm = mcpwm.new({ gpio = 2, frequency_hz = 1000, duty_percent = 50 })`
- A single handle can also drive two outputs with `gpio`/`gpio_a` and `gpio_b`
- Start output with `pwm:start()` or `pwm:set_enabled(true)`
- Change duty cycle on channel 1 with `pwm:set_duty(percent)`
- Change duty cycle on a specific channel with `pwm:set_duty(channel, percent)`
- Change frequency with `pwm:set_frequency(hz)`
- Query the number of outputs on a handle with `pwm:get_channel_count()`
- Stop output with `pwm:stop()` or `pwm:set_enabled(false)`
- Release resources with `pwm:close()`

## Config table
- `gpio` or `gpio_a`: required primary output GPIO
- `gpio_b`: optional secondary output GPIO on the same operator
- `group_id`: optional, defaults to `0`
- `resolution_hz`: optional, defaults to `1000000`
- `frequency_hz`: optional, defaults to `1000`
- `duty_percent`: optional, defaults to `50` for channel 1
- `duty_percent_b`: optional, defaults to `50` for channel 2
- `invert`: optional, defaults to `false` for channel 1
- `invert_b`: optional, defaults to `false` for channel 2

## Example
```lua
local mcpwm = require("mcpwm")

local pwm = mcpwm.new({
    gpio = 2,
    gpio_b = 4,
    frequency_hz = 1000,
    duty_percent = 25,
    duty_percent_b = 75,
})

pwm:start()
pwm:set_duty(75)
pwm:set_duty(2, 40)
pwm:stop()
pwm:close()
```
