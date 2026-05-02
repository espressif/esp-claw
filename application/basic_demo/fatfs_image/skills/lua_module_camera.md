# Lua Camera

This skill describes how to correctly use camera when writing Lua scripts.

## How to call
- Import it with `local camera = require("camera")`
- Call `camera.open(dev_path)` before reading frames
- Call `camera.info()` to get stream information such as `width`, `height`, and `pixel_format`
- Call `camera.get_frame([timeout_ms])` to borrow one raw frame
- Call `camera.release_frame(frame)` or `frame:release()` after using a borrowed frame
- Call `camera.capture(save_path [, timeout_ms])` to capture a frame to a `.jpg` or `.jpeg` path under the current storage root
- Call `camera.close()` when the camera is no longer needed

## Frame lifecycle

`camera.get_frame()` returns a `camera.frame` userdata.

Available methods:
- `frame:data()`: returns the frame as a Lua string
- `frame:ptr()`: returns the frame buffer as `lightuserdata`
- `frame:bytes()`: returns the frame size in bytes
- `frame:info()`: returns a table with `width`, `height`, `pixel_format`, `frame_bytes`, and `timestamp_us`
- `frame:release()`: releases the borrowed frame buffer back to the driver

Important:
- `camera.get_frame()` is a borrowed-buffer API, not a copy API
- You must release every frame before calling `camera.get_frame()` again
- For display preview, prefer `frame:ptr()` and pass it directly to display APIs that accept `lightuserdata`
- `frame:data()` copies the frame into a Lua string, so it is slower and uses more memory

## Example
```lua
local camera = require("camera")
local board_manager = require("board_manager")

local camera_paths = board_manager.get_camera_paths()
camera.open(camera_paths.dev_path)
local info = camera.info()
print(info.width, info.height, info.pixel_format)

local frame = camera.get_frame(1000)
local frame_info = frame:info()
print(frame_info.width, frame_info.height, frame_info.pixel_format, frame:bytes())
frame:release()

local storage = require("storage")
local capture = camera.capture(storage.join_path(storage.get_root_dir(), "capture.jpg"), 3000)
print(capture.path, capture.bytes)
camera.close()
```
