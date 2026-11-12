---
{
  "name": "nullpod_controls",
  "description": "Use the NullPod handheld's five-way keys, rotary encoder, RGB LED, buzzer, battery voltage and IR blaster. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly",
    "tags": [
      "nullpod",
      "handheld",
      "buttons",
      "encoder"
    ]
  }
}
---

# NullPod Controls

Use this skill when the user asks to read the handheld's keys or encoder, light the RGB LED,
beep the buzzer, check the battery level, or send an infrared command.

The board-level devices (`display_lcd`, `lcd_touch`, `fs_sdcard`, `power_key`, `imu_sensor`,
`ir_blaster`) are owned by the board manager. Everything else is wired through the Lua
modules by pin number, as listed below.

## Pin map

| Function | GPIO | Interface | Notes |
|----------|------|-----------|-------|
| Key UP | 16 | button | Active low, internal pull-up |
| Key DOWN | 12 | button | Active low, internal pull-up |
| Key LEFT | 21 | button | Active low, internal pull-up |
| Key RIGHT | 2 | button | Active low, internal pull-up |
| Key CENTER | 0 | button | Active low, also the download-mode strap |
| Encoder A | 9 | knob | Rotary quadrature input |
| Encoder B | 3 | knob | Rotary quadrature input |
| RGB LED | 45 | led_strip | Single WS2812 pixel |
| Buzzer | 46 | mcpwm | Passive buzzer, drive with a square wave |
| Battery sense | 1 | adc | 56 k / 22 k divider, pack = ADC x 78 / 22 |
| IR TX | 42 | ir | 38 kHz carrier, transmit only |
| IMU | I2C 0x68 | imu | BMI270 on the shared bus |

Never drive GPIO17 (PWR_KEY) or GPIO18 (PWR_HOLD). GPIO18 holds the power latch and is owned by
`power_key.c`; pulling it low switches the board off immediately.

## Scripts

Run one script with `lua_run_script` and pass `args` as shown. Prefer these scripts over
writing new ones; they clamp inputs and release hardware handles when they finish.

| Task | Script | Args |
|------|--------|------|
| Watch keys and encoder | `{CUR_SKILL_DIR}/scripts/read_inputs.lua` | `{"duration_ms": 5000}` |
| Set RGB LED | `{CUR_SKILL_DIR}/scripts/set_rgb.lua` | `{"enabled": true, "brightness": 128, "color": {"r": 255, "g": 0, "b": 0}}` |
| Beep | `{CUR_SKILL_DIR}/scripts/beep.lua` | `{"frequency_hz": 2700, "duration_ms": 120, "duty_percent": 50}` |
| Battery | `{CUR_SKILL_DIR}/scripts/battery_status.lua` | `{"samples": 16}` |
| Fire an IR frame | `{CUR_SKILL_DIR}/scripts/ir_probe.lua` | `{"address": 4, "command": 8, "repeat_count": 2}` |

### read_inputs.lua

Polls all six keys plus the encoder for `duration_ms` (100-30000, default 5000) and prints one
JSON object: `{"duration_ms": 5000, "events": [{"source": "key", "key": "up", "event": "single_click"}, ...]}`.
`events` is always an array, so a quiet window prints `"events": []` rather than `{}`.
Key events are `press_down`, `press_up`, `single_click`, `double_click`, `long_press_start`.
Encoder events are `left` and `right` with a cumulative `count`.

Keys report events only while the script runs, so tell the user to press them during the window.
Do not claim a key was pressed if the events array is empty.

### set_rgb.lua

Drives the single WS2812 pixel on GPIO45. `brightness` is 0-255 and scales `color`.
`enabled: false` clears the LED. Report the resulting color to the user.

### beep.lua

Emits a square wave on GPIO46. `frequency_hz` is 100-10000 (default 2700),
`duration_ms` is 10-2000 (default 120), `duty_percent` is 1-100 (default 50).
The script returns only after the tone has finished; do not chain several long beeps in one call.

### battery_status.lua

Averages `samples` (1-64, default 16) ADC readings on GPIO1 and prints
`{"adc_mv": 1120, "battery_mv": 3970, "percent": 78}`. The divider ratio is 78/22, and the
percentage is a linear estimate between 3300 mV (0 %) and 4200 mV (100 %). Below 3400 mV warn the
user that the pack is nearly empty.

### ir_probe.lua

Transmits NEC frames and prints `[nullpod_controls] nec #1 addr=0x0004 cmd=0x0008` for each one.
`address` and `command` are decimal 0-65535 (default 4 / 8), `repeat_count` is 1-4 (default 1),
`gap_ms` is 40-2000 (default 120).

The blaster is transmit-only, so this script never listens for a response and cannot learn a
remote: use `send_nec()` with codes you already know. Success here only proves that the RMT
channel accepted the frame. Do not treat a phone camera as proof either way: most rear cameras
have an IR-cut filter and one NEC frame lasts only about 68 ms. Use a 940 nm receiver such as
VS1838B plus a logic analyser to confirm the light really comes out.

## IMU and IR

Both are declared as board devices, so no pin arguments are needed:

```lua
local imu = require("imu")
local sensor = imu.new()               -- imu_sensor: BMI270 at 0x68
local sample = sensor:read()           -- sample.accel.x/.y/.z, sample.gyro.x/.y/.z
sensor:close()
```

```lua
local ir = require("ir")
local blaster = ir.new()               -- ir_blaster: TX on GPIO42
blaster:send_nec(0x04, 0x08)           -- NEC address + command
blaster:close()
```

`ir.new()` only supports transmission here: there is no receiver and no enable pin on this board,
so an unguarded `dev:receive()` does not simply time out - the module raises
`ir: RX GPIO not configured` and the script aborts. IR learning is not available; use
`ir_probe.lua` (or `send_nec()` directly) instead of the upstream learn-then-replay demos.

## Failure handling

If a script prints an error, report that error text to the user and stop. Do not retry with
different pins, and do not assume a peripheral works when the module failed to open.
