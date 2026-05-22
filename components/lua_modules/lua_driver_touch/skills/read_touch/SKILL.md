---
{
  "name": "read_touch",
  "description": "Read smoothed capacitive touch sensor values from one or more input GPIO channels. Use when the user asks for touch smooth values, capacitive touch channel readings, or smoothed touch data. Requires explicit gpios and board_hardware_info before assigning GPIOs.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Read Touch

Use this skill only to read `smooth` values from capacitive touch input channels.

Run exactly one script with `lua_run_script`.

Read `board_hardware_info` before assigning GPIOs. Verify the requested GPIOs are touch-capable for the target chip and are not occupied by unrelated board hardware.

If `lua_run_script` returns an error, report that error directly to the user. Do not retry with changed GPIOs unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "required": ["gpios"],
  "properties": {
    "gpios": {
      "type": "array",
      "items": {
        "type": "integer",
        "minimum": 0
      },
      "minItems": 1,
      "description": "Touch-capable GPIO numbers to sample."
    },
    "samples": {
      "type": "integer",
      "default": 10,
      "minimum": 1,
      "maximum": 100
    },
    "interval_ms": {
      "type": "integer",
      "default": 200,
      "minimum": 0,
      "maximum": 10000
    }
  }
}
```

## Tool Call Inputs

Read one sample from several touch GPIOs:

```json
{"path":"{CUR_SKILL_DIR}/scripts/read_touch.lua","args":{"gpios":[2,3,4],"samples":1}}
```

Read repeated smooth values:

```json
{"path":"{CUR_SKILL_DIR}/scripts/read_touch.lua","args":{"gpios":[2,3,4,5],"samples":20,"interval_ms":300}}
```

## Recommended Flow

1. Activate `board_hardware_info` before operating touch hardware.
2. Use only explicit `gpios` from the user or from verified board hardware information.
3. Run `{CUR_SKILL_DIR}/scripts/read_touch.lua` with `gpios`, `samples`, and `interval_ms`.
4. Report the returned `smooth` values by sample, GPIO, and touch channel.

## Output Meaning

- `smooth`: smoothed touch sensor data from `TOUCH_CHAN_DATA_TYPE_SMOOTH`.
- `gpio`: input GPIO sampled by the touch driver.
- `channel`: touch sensor channel mapped from the GPIO.
