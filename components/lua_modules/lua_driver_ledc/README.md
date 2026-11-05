# Lua LEDC

`ledc` provides PWM output from Lua for applications such as servo control.

## How to call

- Import it with `local ledc = require("ledc")`
- Create a PWM handle with `local pwm = ledc.new({ gpio = 4, frequency_hz = 50, duty_percent = 7.5 })`
- Start output with `pwm:start()`
- Change duty cycle with `pwm:set_duty(percent)`
- Change frequency with `pwm:set_frequency(hz)`
- Read current resources and hardware settings with `local info = pwm:get_info()`
- Stop output with `pwm:stop()`
- Release resources with `pwm:close()`

`pwm:stop()` stops output but keeps the GPIO, channel, and timer in use. Use `pwm:close()` for prompt release; Lua GC and job exit also finalize handles. Retry a failed explicit close before dropping the handle. Compatible handles can share a timer; changing its frequency is rejected while another handle uses it.

## Config table

- `gpio`: required output GPIO
- `frequency_hz`: required positive PWM frequency in Hz
- `duty_percent`: optional, defaults to `50`
- `duty_resolution_bits`: optional, defaults to `14`

On boards with camera support, available LEDC timers/channels may be reduced. Creating a PWM handle returns an error if no suitable LEDC resource is available.

## Current information

`pwm:get_info()` returns a snapshot without changing the output or resource ownership:

| Field | Description |
| --- | --- |
| `gpio` | Output GPIO |
| `channel` | Assigned LEDC channel, numbered from 0 |
| `timer` | Assigned LEDC timer, numbered from 0 |
| `frequency` | Actual frequency in Hz; may differ slightly from the requested value |
| `resolution` | Duty resolution in bits |
| `duty` | Current hardware duty count, not a percentage |
| `timer_ref_count` | Number of Lua LEDC handles using this timer at query time; frequency changes are rejected when greater than 1 |

Information is available after creation, before starting, and after `stop()`. Duty changes made while stopped take effect on the next `start()`. The returned `duty` excludes pending changes and does not indicate whether output is active. A closed handle or a hardware read failure raises a Lua error.

## Example

```lua
local ledc = require("ledc")

local pwm = ledc.new({
    gpio = 4,
    frequency_hz = 50,
    duty_percent = 7.5,
})

pwm:start()
local info = pwm:get_info()
print(info.gpio, info.channel, info.timer, info.frequency, info.duty, info.timer_ref_count)
pwm:set_duty(12.5)
pwm:stop()
pwm:close()
```
