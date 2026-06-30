---
{
  "name": "take_picture",
  "description": "Take one photo with the board camera and save it as a JPEG file. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Take Picture

Use this skill when the user asks to take a photo, capture a picture, snap an image, or save a camera still from the board camera.

Run exactly one script with `lua_run_script` after reading `board_hardware_info`.

If `lua_run_script` returns an error, report that error directly to the user.
Do not retry with changed arguments or run another camera script in the same turn unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "filename": {
      "type": "string",
      "default": "capture.jpg",
      "description": "JPEG filename to create under the storage root or under dir."
    },
    "dir": {
      "type": "string",
      "default": "",
      "description": "Optional single directory name under the storage root."
    },
    "timeout_ms": {
      "type": "integer",
      "default": 3000,
      "minimum": 0
    },
    "skip_frames": {
      "type": "integer",
      "default": 3,
      "minimum": 0,
      "description": "Number of warm-up frames to discard before saving the photo."
    }
  }
}
```

Path rules:
- `filename` must be a simple `.jpg` or `.jpeg` filename.
- `filename` must not contain `/`, `\`, or `..`.
- `dir` is optional and must be a single directory name under the storage root.
- `dir` must not contain `/`, `\`, or `..`.

## Tool Call Inputs

Take a photo with the default output name:

```json
{"path":"{CUR_SKILL_DIR}/scripts/take_picture.lua","args":{}}
```

Take a photo with a custom filename:

```json
{"path":"{CUR_SKILL_DIR}/scripts/take_picture.lua","args":{"filename":"photo.jpg"}}
```

Take a photo into a storage-root-relative directory:

```json
{"path":"{CUR_SKILL_DIR}/scripts/take_picture.lua","args":{"dir":"photos","filename":"latest.jpg","timeout_ms":5000}}
```

Take a photo after discarding more warm-up frames:

```json
{"path":"{CUR_SKILL_DIR}/scripts/take_picture.lua","args":{"filename":"stable.jpg","skip_frames":5}}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and confirm that a `camera` device is listed.
2. If no camera is listed, tell the user that the board does not declare a camera and stop.
3. Choose a safe filename. Use the default unless the user requested a specific output name.
4. Run `{CUR_SKILL_DIR}/scripts/take_picture.lua` with the selected `args`.
5. Report the saved path, byte count, resolution, pixel format, skipped warm-up frames, and any error directly from the script output.
