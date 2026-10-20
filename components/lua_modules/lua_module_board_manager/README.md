# Lua Board Manager

`board_manager` provides board metadata and camera paths.

## API

- `board_manager.get_board_info()` returns `{ name, chip, version, description, manufacturer }`.
- `board_manager.get_camera_paths()` returns `{ dev_path, meta_path }`; `meta_path` may be absent.

Both functions take no arguments and return `nil, err` when the requested information is unavailable. Missing board metadata fields are returned as empty strings.

`get_camera_paths()` queries the board camera only. The camera must already be initialized by the firmware; this call does not initialize it. Unsupported or uninitialized cameras return `nil, err`. Use `camera.list_devices()` to discover available capture devices, including external cameras.

This module only reads information. Use `display`, `lvgl`, `audio`, `camera`, or the appropriate sensor module to access hardware.

## Example

```lua
local board_manager = require("board_manager")

local info = assert(board_manager.get_board_info())
print(info.name, info.chip, info.version)

local paths, err = board_manager.get_camera_paths()
if paths then
    print(paths.dev_path)
else
    print(err)
end
```
