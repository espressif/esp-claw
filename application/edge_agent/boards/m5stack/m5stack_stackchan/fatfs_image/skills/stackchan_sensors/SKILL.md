---
{
  "name": "stackchan_sensors",
  "description": "Read the StackChan robot's ambient light sensor and battery: how bright or dark the room is (lux), and battery voltage, current and whether it is charging or discharging. Covers 光线/亮度/环境光/多亮/暗不暗/lux and 电量/电池/电压/电流/充电中. These two chips are driven from Lua and do not appear in board_hardware_info's device inventory. StackChan board only.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# StackChan Sensors

Read this board's two Lua-driven sensors. Run exactly one script with `lua_run_script`: `{CUR_SKILL_DIR}/scripts/sensors.lua`.

**These chips are real but are not in `board_hardware_info`.** That inventory only lists devices declared in `board_devices.yaml`, and these two are driven from Lua by design, so its absence of a light sensor is not evidence that the board lacks one. This skill is the authority for them:

| Chip | I2C | Reports |
|------|-----|---------|
| LTR-553ALS (on the CoreS3 itself) | `0x23` | ambient light in lux, raw CH0/CH1, optional proximity |
| INA226 (on the Power board, 10 mΩ shunt) | `0x41` | battery voltage, current, power, charging direction |

## Actions

`action` selects what to read: `all` (default), `light`, or `power`.

Set `samples` above 1 to watch a value change over time — useful for "is it getting darker" or for catching a charger being plugged in. `interval_ms` is the gap between samples.

`proximity: true` also enables the LTR-553's proximity channel. It is off by default because it drives an IR LED and only reports a relative count, not a distance.

## Reading The Results

- **Light**: the script prints the lux value plus a plain-language band (dark / dim / normal indoor / bright / very bright). Use that band rather than inventing your own thresholds. The part tracks relative light well, but its absolute scale has not been calibrated on this board, so treat the lux number as approximate.
- **Battery**: **current is positive while discharging** on this board and negative while charging, confirmed on hardware. The script states the direction in words, so use that rather than reasoning about the sign yourself.
- **There is no battery percentage.** This board has no state-of-charge curve. Report the voltage and the charge direction; do not convert voltage into a percentage, and do not guess "about N% left".
- If one chip does not answer, the script says which address failed and still reports the other one. Report that partial result plus which sensor is missing.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "action": {"type": "string", "enum": ["all", "light", "power"], "default": "all"},
    "samples": {"type": "integer", "default": 1, "minimum": 1, "maximum": 20},
    "interval_ms": {"type": "integer", "default": 300, "minimum": 0, "maximum": 5000},
    "proximity": {"type": "boolean", "default": false}
  }
}
```

## Tool Call Inputs

Both sensors once:

```json
{"path":"{CUR_SKILL_DIR}/scripts/sensors.lua","args":{}}
```

How bright is it:

```json
{"path":"{CUR_SKILL_DIR}/scripts/sensors.lua","args":{"action":"light"}}
```

Battery state:

```json
{"path":"{CUR_SKILL_DIR}/scripts/sensors.lua","args":{"action":"power"}}
```

Watch the light change over about three seconds:

```json
{"path":"{CUR_SKILL_DIR}/scripts/sensors.lua","args":{"action":"light","samples":10,"interval_ms":300}}
```

## Failures

Report script errors as they are; do not retry with different arguments.

- **One address did not answer** — that chip or its wiring. The other reading is still valid.
- **Neither answered** — the I2C bus, not the chips. `/system/scripts/stackchan_5v_check.lua` and the `builtin_lua_modules` bus scripts diagnose that. Note that a bus scan on this board reports phantom devices and misses real ones, so trust each chip's own identity register instead.
