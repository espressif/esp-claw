# Lua MCPWM

This skill describes how to use `mcpwm` from Lua for generic PWM output.

## How to call
- Import it with `local mcpwm = require("mcpwm")`
- Create a PWM handle with `local pwm = mcpwm.new({ gpio = 2, frequency_hz = 1000, duty_percent = 50 })`
- Start output with `pwm:start()` or `pwm:set_enabled(true)`
- Change duty cycle with `pwm:set_duty(percent)`
- Change frequency with `pwm:set_frequency(hz)`
- Stop output with `pwm:stop()` or `pwm:set_enabled(false)`
- Release resources with `pwm:close()`

## Config table
- `gpio`: required output GPIO
- `group_id`: optional, defaults to `0`
- `resolution_hz`: optional, defaults to `1000000`
- `frequency_hz`: optional, defaults to `1000`
- `duty_percent`: optional, defaults to `50`
- `invert`: optional, defaults to `false`

## Example
```lua
local mcpwm = require("mcpwm")

local pwm = mcpwm.new({
    gpio = 2,
    frequency_hz = 1000,
    duty_percent = 25,
})

pwm:start()
pwm:set_duty(75)
pwm:stop()
pwm:close()
```
