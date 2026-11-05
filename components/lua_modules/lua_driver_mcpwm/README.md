# Lua MCPWM

`require("mcpwm")` provides one or two PWM outputs with a shared frequency.

## API

| Call | Result / behavior |
| --- | --- |
| `mcpwm.new(config)` | Returns a handle with output stopped and pins low. |
| `pwm:start()` | Starts output using the configured duty. |
| `pwm:stop()` | Stops output and drives pins low, including inverted outputs. |
| `pwm:set_enabled(boolean)` | Equivalent to `start()` or `stop()`. |
| `pwm:set_duty(percent)` | Sets channel 1 duty; while stopped, saves it for the next start. |
| `pwm:set_duty(channel, percent)` | Sets duty for channel `1` or `2`; channel 2 requires `gpio_b`. |
| `pwm:set_frequency(hz)` | Changes both channels' frequency; running outputs briefly stop and restart. |
| `pwm:get_channel_count()` | Returns `1` or `2`. |
| `pwm:close()` | Stops output and releases resources; repeated close is allowed. |

Except for `new()` and `get_channel_count()`, calls return no values on success. Invalid arguments and driver failures raise Lua errors. After a failed `close()`, only another `close()` is allowed.

## Configuration

| Field | Meaning | Default |
| --- | --- | --- |
| `gpio` / `gpio_a` | Primary output GPIO (`gpio` takes precedence) | Required |
| `gpio_b` | Secondary output GPIO, different from the primary GPIO | None |
| `group_id` | MCPWM hardware group | `0` |
| `resolution_hz` | Requested timer resolution | `1000000` |
| `frequency_hz` | Output frequency | `1000` |
| `duty_percent` / `duty_percent_b` | Channel 1 / 2 duty before inversion | `50` |
| `invert` / `invert_b` | Channel 1 / 2 output inversion; boolean | `false` |

Use available output-capable GPIOs. Duty must be finite and within `0..100`. Frequency and resolution must be positive integers within Lua's integer range and `uint32_t`; each period must contain at least two hardware ticks and fit the timer. Actual frequency and duty are limited by hardware resolution.

## Usage constraints

- Start, stop, and transitions from 0%/100% to intermediate duty can block for a PWM period plus scheduler delay. Frequency changes introduce an output gap; transitions are not guaranteed glitch-free or phase-continuous.
- Hardware update failures may leave output stopped. If frequency changes successfully but restarting fails, the new frequency remains configured.
- Use a handle within its owning Lua runtime. Call `close()` when finished; job shutdown provides fallback cleanup. Available groups and handles depend on the chip.

## Example

```lua
local mcpwm = require("mcpwm")
local pwm = mcpwm.new({gpio = 2, gpio_b = 4, frequency_hz = 1000, duty_percent = 25, duty_percent_b = 75})
pwm:start()
pwm:set_duty(50)
pwm:set_duty(2, 40)
pwm:stop()
pwm:close()
```

Use free GPIOs suitable for the connected hardware. Occupied or unavailable resources raise an error. If `close()` fails, retry it before reusing the resources.
