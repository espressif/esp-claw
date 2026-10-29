# lib_scs_servo.lua

Reusable Lua driver for Feetech SCS serial-bus servos (SCSCL memory table). It uses the builtin `uart` module and exports `require("lib_scs_servo")`.

## When to use

Use this library when a board has Feetech SCS-series bus servos wired to a UART — several servos share one TX/RX pair and are addressed by id. On M5Stack StackChan the bus is UART port `1`, `tx = GPIO6`, `rx = GPIO7` at 1 Mbps, with the yaw servo at id `1` and the pitch servo at id `2`.

This library speaks raw servo units. It intentionally holds no notion of a home position or of degrees, because those are properties of the machine the servos are bolted into, not of the servo.

## Loading

```lua
local scs_servo = require("lib_scs_servo")
```

The script must also have access to the `uart` module.

## Constructor

```lua
local bus = scs_servo.new(opts)
```

`opts` is a table:

- `uart`: existing UART handle. Use this when the script already opened the port.
- `port`: UART port number. Required if `uart` is not provided. Use `1` or higher; port `0` is the log console.
- `tx`: TX GPIO. Required if `uart` is not provided.
- `rx`: RX GPIO. Required if `uart` is not provided.
- `baud`: baud rate. Defaults to `1000000`, the SCS factory setting.
- `timeout_ms`: per-reply timeout. Defaults to `20`.
- `pos_max`: bus-wide fallback upper bound for position commands. Defaults to `1023`, the SCSCL encoder range.
- `pos_limits`: table of per-id travel limits, e.g. `{[1] = {200, 800}, [2] = {683, 971}}`.
- `close_uart`: set to `false` to keep a library-created port open past `bus:close()`.

## Position limits

The encoder spans `0..1023` counts (about 320° at 0.3125°/count), but the machine a servo is bolted into usually allows less. Nothing in the protocol stops a caller from commanding a position past the mechanical stop, where the servo stalls, draws heavy current and heats up.

- `bus:set_pos_limits(id, min_pos, max_pos)`
- `bus:get_pos_limits(id)`: returns `min, max`, or `nil` when only the bus-wide fallback applies.
- `bus:clear_pos_limits(id)`

`write_pos`, `reg_write_pos` and `sync_write_pos` **clamp** to these limits rather than failing: a control loop that overshoots should be pinned to the safe end, not made to error. `write_pos` and `reg_write_pos` return `true, position_actually_sent` so a caller can tell that clamping happened.

Limits are per id because they are a property of the machine, not of the servo — they are "zero point plus mechanical range", and the zero point differs between units. Measure them rather than copying numbers from another machine.

## Error handling

Bad arguments raise a Lua error, because they are programming mistakes. Bus failures return `nil, err` instead, because a servo that is unplugged or whose power rail is off is an ordinary runtime condition. `err` is one of `"no reply"`, `"id mismatch: ..."`, `"length mismatch: ..."`, `"truncated payload"`, `"missing checksum"` or `"checksum mismatch"`.

Write calls return `true` on success.

## Module members

- `scs_servo.REG`: the SCSCL memory table, for raw register access.
- `scs_servo.BROADCAST_ID`: `0xFE`. Frames sent to it are not acknowledged.
- `scs_servo.MODE_POSITION`, `scs_servo.MODE_PWM`.

## Discovery

- `bus:ping(id)`: returns `id` when the servo answers, otherwise `nil, err`.
- `bus:scan([first, last])`: pings each id in the range (default `0`..`253`) and returns an array of the ones that answered. This takes `timeout_ms` per absent id, so a full sweep is slow.

## Position mode

- `bus:write_pos(id, position, move_time, speed)`: `position` is `0`..`65535` in servo counts. `move_time` is the time budget for the move and `speed` a speed cap; `0` means unlimited. Both default to `0`.
- `bus:reg_write_pos(id, position, move_time, speed)`: queue a move without starting it.
- `bus:action([id])`: release queued moves. Defaults to broadcast.
- `bus:sync_write_pos(ids, positions, [times], [speeds])`: one broadcast frame that moves several servos together, so they start on the same instant. `times` and `speeds` may be omitted. Returns `true`; broadcast frames are never acknowledged, so this cannot report a servo-side failure.

## Feedback

- `bus:read_pos(id)`: present position in counts.
- `bus:read_speed(id)`: present speed, signed.
- `bus:read_load(id)`: present load, signed.
- `bus:read_current(id)`: present current, signed.
- `bus:read_voltage(id)`: present voltage byte.
- `bus:read_temperature(id)`: present temperature byte, degrees Celsius.
- `bus:read_moving(id)`: non-zero while the servo is still travelling.
- `bus:read_feedback(id)`: one 15-byte burst read covering registers 56..70. Returns a table with `position`, `speed`, `load`, `voltage`, `temperature`, `moving` and `current`. Prefer this in a control loop: it is one round trip instead of six.

Speed, load and current are reported as sign-magnitude, not two's complement; the library converts them to ordinary signed Lua numbers.

## Torque

- `bus:enable_torque(id, enabled)`
- `bus:read_torque_enable(id)`: returns a boolean.

## Angle limits and PWM mode

The SCSCL has no mode register. PWM mode is entered by zeroing the min/max angle limits, which live in **EPROM**. Switching modes therefore writes EPROM every time, so do not do it inside a loop.

- `bus:read_angle_limits(id)`: returns `min, max`.
- `bus:write_angle_limits(id, min, max)`
- `bus:pwm_mode(id)`: zero both limits directly.
- `bus:switch_mode(id, mode)`: `mode` is `scs_servo.MODE_POSITION` or `scs_servo.MODE_PWM`. Entering PWM mode caches the current limits so returning to position mode can restore them, and returns `true, min, max`. Returning to position mode without having entered PWM mode through this call returns `nil, "no cached angle limits; ..."` — use `write_angle_limits` with known values instead.

> **A servo left in PWM mode has no travel protection, and the zeroed limits survive a reboot.** If the script that entered PWM mode dies before switching back, the servo will accept any position command afterwards. Restore the limits with `write_angle_limits(id, min, max)` using the values `switch_mode` returned, and persist them somewhere if that matters — the in-library cache only lives as long as the handle. The `set_pos_limits` clamp is host-side and still applies, but it cannot protect a servo driven by other software.
- `bus:write_pwm(id, pwm)`: `-1023`..`1023`. Only meaningful in PWM mode. Direction is encoded in bit 10 of the register, which the library handles.

## EPROM lock

- `bus:unlock_eprom(id)` / `bus:lock_eprom(id)`: unlock before writing EPROM registers such as the servo id or baud rate, and lock again afterwards.

## Raw register access

- `bus:read_byte(id, mem_addr)` / `bus:read_word(id, mem_addr)` / `bus:read_bytes(id, mem_addr, len)`
- `bus:write_byte(id, mem_addr, value)` / `bus:write_word(id, mem_addr, value)` / `bus:write_bytes(id, mem_addr, bytes)`

16-bit values are big endian on the wire; `read_word` and `write_word` handle that.

## Teardown

- `bus:close()`: closes the UART when the library opened it.

## Example: move the StackChan head

```lua
local scs_servo = require("lib_scs_servo")
local delay = require("delay")

local bus = scs_servo.new({port = 1, tx = 6, rx = 7, baud = 1000000})

-- The servo power rail must already be up. On StackChan the firmware raises it
-- during board init via the PY32 expander's VM_EN pin.
if not bus:ping(1) then
    print("yaw servo not responding; is the VM rail on?")
    bus:close()
    return
end

local YAW, PITCH = 1, 2
bus:enable_torque(YAW, true)
bus:enable_torque(PITCH, true)

-- Move both axes together, 20 time units per move.
bus:sync_write_pos({YAW, PITCH}, {460, 620}, {20, 20})
delay.delay_ms(500)

local fb = bus:read_feedback(YAW)
if fb then
    print(string.format("yaw pos=%d load=%d temp=%dC", fb.position, fb.load, fb.temperature))
end

bus:close()
```

## Notes

- Frames are `FF FF id len inst [addr] [params] checksum`, with the checksum being the bitwise NOT of the sum of everything after the preamble. Replies are `FF FF id len status [data] checksum`.
- The reply parser tolerates up to a dozen stray bytes before the preamble, then validates id, length and checksum. Anything unexpected becomes an `err` string rather than a partial reading.
- The library flushes the RX buffer before every request, so a stale reply from an earlier timeout cannot be mistaken for the current one.
- Broadcast id `0xFE` never produces a reply, so calls that use it cannot detect failure.
