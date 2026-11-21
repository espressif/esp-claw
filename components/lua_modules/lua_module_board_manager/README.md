# Lua Board Manager

`board_manager` provides board metadata and camera paths.

## API

- `board_manager.get_board_info()` returns `{ name, chip, version, description, manufacturer }`.
- `board_manager.get_camera_paths()` returns `{ dev_path, meta_path }`; `meta_path` may be absent.
- `board_manager.get_display_shape()` returns `{ shape, corner_radius }`. `shape` is `rect`, `rounded_rect` or `circle`; `corner_radius` is in pixels and is `0` unless `shape` is `rounded_rect`.

All functions take no arguments. `get_board_info()` and `get_camera_paths()` return `nil, err` when the requested information is unavailable. Missing board metadata fields are returned as empty strings.

`get_display_shape()` always succeeds. Boards declare the shape with `CONFIG_LUA_MODULE_BOARD_MANAGER_DISPLAY_SHAPE_*` and `CONFIG_LUA_MODULE_BOARD_MANAGER_DISPLAY_CORNER_RADIUS` in `sdkconfig.defaults.board`; boards that do not set it report `rect`. For layout math such as visible spans and safe areas, use the `display_shape` library documented in `lib/display_shape.md`.

`get_camera_paths()` queries the board camera only. The camera must already be initialized by the firmware; this call does not initialize it. Unsupported or uninitialized cameras return `nil, err`. Use `camera.list_devices()` to discover available capture devices, including external cameras.

This module only reads information. Use `display`, `lvgl`, `audio`, `camera`, or the appropriate sensor module to access hardware.

## Example

```lua
local board_manager = require("board_manager")

local info = assert(board_manager.get_board_info())
print(info.name, info.chip, info.version)

local shape = board_manager.get_display_shape()
print(shape.shape, shape.corner_radius)

local paths, err = board_manager.get_camera_paths()
if paths then
    print(paths.dev_path)
else
    print(err)
end
```
