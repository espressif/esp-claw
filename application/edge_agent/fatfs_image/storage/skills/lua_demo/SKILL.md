---
{
  "name": "lua_demo",
  "description": "Run bundled Lua applications and demos. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Lua Demo

Use this skill when the user wants to run an application, demo, game, visualizer, preview, or interactive Lua program on the board.

Run exactly one script with `lua_run_script` unless the user explicitly asks to run another application.

If `lua_run_script` returns an error, report that error directly to the user. Do not retry with another application unless the user asks.

## Applications

### LCD Touch Paint

Use when the user asks for a drawing app, paint app, touch test, LCD touch demo, or finger drawing.

Requires a display and LCD touch device.

Tool call:
```json
{"path":"{CUR_SKILL_DIR}/scripts/lcd_touch_paint.lua","args":{}}
```

### Flappy Bird

Use when the user asks to play a game, run Flappy Bird, start a bird game, or launch an interactive game demo.

Requires a display. Uses LCD touch when available; otherwise it falls back to a button. Audio is optional.

Tool call:
```json
{"path":"{CUR_SKILL_DIR}/scripts/flappybird.lua","args":{}}
```

### Audio FFT

Use when the user asks for an audio visualizer, microphone visualizer, sound-reactive LEDs, FFT demo, or volume meter.

Requires an audio input device and an LED strip compatible with the script wiring.

Tool call:
```json
{"path":"{CUR_SKILL_DIR}/scripts/audio_fft.lua","args":{}}
```

### Camera Preview

Use when the user asks to preview the camera, show camera output, open the camera, or run a camera demo.

Requires a camera and display. The script supports RGB565/RGB565X preview frames.

Tool call:
```json
{"path":"{CUR_SKILL_DIR}/scripts/camera_preview.lua","args":{}}
```

### Clock Dial Demo

Use when the user asks for a clock, watch face, dial, time display, or clock demo.

Requires a display.

Tool call:
```json
{"path":"{CUR_SKILL_DIR}/scripts/clock_dial_demo.lua","args":{}}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and check that the requested application's required hardware is declared.
2. Choose the script whose application best matches the user's request.
3. If the user asks for a generic application or demo without naming one, prefer `flappybird.lua` for a game request.
4. Prefer `clock_dial_demo.lua` for a generic display demo request.
5. If required hardware is not declared, tell the user which hardware is missing and do not run the script.
6. Run the selected script with `lua_run_script` and an empty `args` object.
7. Report the result or error directly to the user.
