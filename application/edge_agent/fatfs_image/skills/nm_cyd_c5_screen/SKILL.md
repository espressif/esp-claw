---
{
  "name": "nm_cyd_c5_screen",
  "description": "Control the NM-CYD-C5 ST7789 LCD content: fill the screen with a color, show centered text, display a message, or clear the screen for a visible duration.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# NM-CYD-C5 Screen

Use this skill when the user asks to show text, display a message, set the screen color, fill the LCD, clear the display, or otherwise draw content on the NM-CYD-C5 board screen.

Do not use this skill for backlight brightness, screen off, dimming, or closing the backlight. Those requests belong to `nm_cyd_c5_backlight`.

## Tool Call

Always use synchronous `lua_run_script`:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_screen.lua","args":{}}
```

Do not use `lua_run_script_async`; the script intentionally waits for `duration_ms` so the user can actually see the screen before the emote app reclaims display ownership.

## Args

- `mode`: `color`, `text`, `message`, or `clear`; aliases include `fill`, `bg`, `label`, `msg`, `notify`, `off`, `blank`.
- `color`: named color for `mode=color`: `red`, `green`, `blue`, `white`, `black`, `yellow`, `cyan`, `magenta`, `orange`, `purple`, `pink`, `gray`.
- `r`, `g`, `b`: explicit RGB values for `mode=color`.
- `text`: text to draw for `mode=text` or `mode=message`.
- `title`: title bar for `mode=message`.
- `bg`, `fg`: background and foreground colors for text/message modes.
- `font_size`, `title_font_size`: integer font sizes.
- `duration_ms`: visible duration; default is `3000` for color, `5000` for text/message, `1000` for clear.

## Examples

Purple screen for 5 seconds:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_screen.lua","args":{"mode":"color","color":"purple","duration_ms":5000}}
```

Show centered text:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_screen.lua","args":{"mode":"text","text":"Hello","bg":"blue","duration_ms":5000}}
```

Show a status message:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_screen.lua","args":{"mode":"message","title":"Status","text":"WiFi OK","bg":"black","duration_ms":4000}}
```

Clear briefly:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_cyd_c5_screen.lua","args":{"mode":"clear","duration_ms":1000}}
```

## Response Rules

Run exactly one `lua_run_script` call unless the user explicitly asks for a sequence. Report script errors directly and do not retry with generic display scripts.