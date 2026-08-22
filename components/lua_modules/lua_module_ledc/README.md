# Lua LEDC

This module describes how to use `ledc` from Lua for generic PWM output such as
servo control.

## How to call

- Import it with `local ledc = require("ledc")`
- Create a PWM handle with `local pwm = ledc.new({ gpio = 4, frequency_hz = 50, duty_percent = 7.5 })`
- Start output with `pwm:start()`
- Change duty cycle with `pwm:set_duty(percent)`
- Change frequency with `pwm:set_frequency(hz)`
- Stop output with `pwm:stop()`
- Release resources with `pwm:close()`

## Config table

- `gpio`: required output GPIO
- `frequency_hz`: required positive PWM frequency in Hz
- `duty_percent`: optional, defaults to `50`
- `duty_resolution_bits`: optional, defaults to `14`

On boards with camera support, available LEDC timers/channels may be reduced. Creating a PWM handle returns an error if no suitable LEDC resource is available.

## Example

```lua
local ledc = require("ledc")

local pwm = ledc.new({
    gpio = 4,
    frequency_hz = 50,
    duty_percent = 7.5,
})

pwm:start()
pwm:set_duty(12.5)
pwm:stop()
pwm:close()
```
