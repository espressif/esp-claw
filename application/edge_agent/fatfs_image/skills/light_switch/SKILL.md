---
{
  "name": "light_switch",
  "description": "Turn a board light on or off, set LED strip color or brightness, and control GPIO lights. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Light Switch

Use this skill when the user asks to turn a board light on or off, change an LED strip color, or adjust light brightness.

Run exactly one script with `lua_run_script` after reading `board_hardware_info`.

You MUST pass args when using `lua_run_script` to control the light, as specified in the input schema shown below in the section `LED Strip Script Args Schema` and `GPIO Light Script Args Schema`.

If `lua_run_script` returns an error, report that error directly to the user.
Do not retry, change arguments, or run another light script in the same turn unless the user explicitly asks.

## LED Strip Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "io": {
      "type": "integer",
      "default": 38,
      "minimum": 0
    },
    "led_count": {
      "type": "integer",
      "default": 1,
      "minimum": 1
    },
    "enabled": {
      "type": "boolean",
      "default": true
    },
    "brightness": {
      "type": "integer",
      "default": 255,
      "minimum": 0,
      "maximum": 255
    },
    "color": {
      "type": "object",
      "properties": {
        "r": {
          "type": "integer",
          "default": 255,
          "minimum": 0,
          "maximum": 255
        },
        "g": {
          "type": "integer",
          "default": 255,
          "minimum": 0,
          "maximum": 255
        },
        "b": {
          "type": "integer",
          "default": 255,
          "minimum": 0,
          "maximum": 255
        }
      }
    }
  }
}
```

## GPIO Light Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "io": {
      "type": "integer",
      "default": 38,
      "minimum": 0
    },
    "enabled": {
      "type": "boolean",
      "default": true
    },
    "active_level": {
      "type": "integer",
      "default": 1,
      "minimum": 0,
      "maximum": 1
    },
    "brightness": {
      "type": "integer",
      "default": 255,
      "minimum": 0,
      "maximum": 255
    }
  }
}
```

GPIO lights are binary. Ignore color for GPIO lights. Treat `brightness: 0` as off and any non-zero brightness as on.

## LED Strip Tool Call Inputs

Use `args` as the JSON object shown below.

Turn off:
```json
{"path":"{CUR_SKILL_DIR}/scripts/led_strip_switch.lua","args":{"enabled":false}}
```

Turn on white:
```json
{"path":"{CUR_SKILL_DIR}/scripts/led_strip_switch.lua","args":{"enabled":true}}
```

Set red at half brightness:
```json
{"path":"{CUR_SKILL_DIR}/scripts/led_strip_switch.lua","args":{"enabled":true,"brightness":128,"color":{"r":255,"g":0,"b":0}}}
```

Set all pixels in a 3-pixel LED strip to red at half brightness:
```json
{"path":"{CUR_SKILL_DIR}/scripts/led_strip_switch.lua","args":{"io":46,"led_count":3,"enabled":true,"brightness":128,"color":{"r":255,"g":0,"b":0}}}
```

## GPIO Light Tool Call Inputs

Turn off an active-high GPIO light:
```json
{"path":"{CUR_SKILL_DIR}/scripts/gpio_light_switch.lua","args":{"io":23,"enabled":false,"active_level":1}}
```

Turn on an active-low GPIO light:
```json
{"path":"{CUR_SKILL_DIR}/scripts/gpio_light_switch.lua","args":{"io":43,"enabled":true,"active_level":0}}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and query the board hardware info before operating hardware.
2. Use the `Device Inventory` headings and occupied IO lines from `board_hardware_info`; do not guess pins outside that skill.
3. Prefer an `led_strip` device when present. Use its occupied IO, typically the referenced RMT TX `gpio` line, as `io`.
4. Use `max_leds` from hardware info as `led_count` when available; otherwise keep the default `led_count: 1`.
5. If no `led_strip` device exists, look for a GPIO light by device name, such as `flashlight`, `led_*`, `*_led`, or names containing `light`.
6. If `chip: gpio_led` or `type: gpio_ctrl` is visible in hardware info, treat that as a GPIO light signal when the name also indicates a light.
7. For GPIO lights, use the GPIO from the device occupied IO such as `gpio` or `pin`.
8. Use `active_level` from hardware info when available. If explicit active-high or active-low text is visible, map it to `1` or `0`.
9. If no active-level information is available, use the script default `active_level: 1` and do not claim active-low behavior.
10. Do not use the default `io` value if `board_hardware_info` provides one.
11. For LED strips, run `{CUR_SKILL_DIR}/scripts/led_strip_switch.lua` with the resolved `io`, `led_count`, and any requested color or brightness.
12. For GPIO lights, run `{CUR_SKILL_DIR}/scripts/gpio_light_switch.lua` with the resolved `io`, `active_level`, and requested enabled state.
13. For GPIO lights, ignore color and map `brightness: 0` to off.
14. If neither LED strip nor GPIO light hardware is listed, inform the user that the board does not declare a controllable light and stop.
15. Report the result or error directly to the user.
