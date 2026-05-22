---
{
  "name": "nm_cyd_c5_rgb",
  "description": "Control the NM-CYD-C5 on-board WS2812 RGB LED on GPIO27: turn it on or off, set colors such as purple, blink or flash for a duration, run rainbow effects, and adjust LED brightness.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# NM-CYD-C5 RGB LED

Use this skill for any request about the NM-CYD-C5 on-board RGB LED, LED light, LED strip, lamp, color light, blink, flash, breathing light, rainbow, or phrases such as `LED灯设置为紫色，闪烁30秒`.

This board has a single WS2812 RGB LED wired to GPIO27. Do not activate `light_switch`, do not inspect board hardware, and do not write ad-hoc `led_strip` or GPIO Lua code for this board LED. Use the canonical script directly.

## Tool Call

Call `lua_run_script` with:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua","args":{}}
```

## Args

- `mode`: `solid`, `off`, `blink`, `rainbow`; aliases include `on`, `set`, `color`, `clear`, `flash`, `sweep`, `demo`.
- `color`: `red`, `green`, `blue`, `white`, `yellow`, `cyan`, `magenta`, `orange`, `purple`, `pink`, `black`, `off`.
- `r`, `g`, `b`: explicit RGB values, each `0..255`, higher priority than `color`.
- `hue`, `sat`, `val`: HSV values, with `hue=0..359`, `sat=0..255`, `val=0..255`.
- `brightness`: named-color brightness scale `0..255`; default is `64`.
- `count`: blink count, default `3`.
- `on_ms`, `off_ms`: blink timing in milliseconds, defaults `200` and `200`.
- `duration_ms`, `step_ms`: rainbow duration and step timing.

## Direct Examples

Purple solid:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua","args":{"color":"purple"}}
```

Turn off:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua","args":{"mode":"off"}}
```

Purple blink for about 30 seconds:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua","args":{"mode":"blink","color":"purple","count":60,"on_ms":250,"off_ms":250}}
```

Blue blink 5 times:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua","args":{"mode":"blink","color":"blue","count":5,"on_ms":200,"off_ms":200}}
```

Two-second rainbow:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua","args":{"mode":"rainbow","duration_ms":2000}}
```

## Duration Mapping

For blink requests that specify seconds, compute `count = seconds * 1000 / (on_ms + off_ms)` and use integer count. For `闪烁30秒`, prefer `on_ms=250`, `off_ms=250`, `count=60`.

## Response Rules

Run exactly one `lua_run_script` call. After it returns successfully, briefly confirm what changed. If it returns an error, report the error and stop.