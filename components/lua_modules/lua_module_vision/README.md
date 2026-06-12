# lua_module_vision

Lua vision modules backed by `image.frame` buffers.

## Modules

- `motion_detect`: compares two `image.frame` objects and returns the number of changed sample points. Enabled by default with `LUA_MODULE_VISION_MOTION_DETECT`.
- `espdet`: runs ESP-DL ESPDet object detection from Lua with a user-provided `.espdl` model file. Enable with `LUA_MODULE_VISION_ESPDET`.

All functions read the frame only during the call. Release frames with `frame:release()` after the vision call returns.
Frame conversion is handled by the shared `image` module, so Lua scripts pass the frame object directly instead of selecting RGB/YUV/GRAY conversion paths.

## Examples

```lua
local camera = require("camera")
local display = require("display")
local image = require("image")
local motion = require("motion_detect")

camera.open("/dev/video0")

local f1 = camera.get_frame()
local first = motion.detect(f1, { stride = 4, pixel_threshold = 0.04, moving_threshold = 0.03 })
f1:release()

local f2 = camera.get_frame()
local m = motion.detect(f2, { stride = 4, pixel_threshold = 0.04, moving_threshold = 0.03 })
print("moving ratio", m.moving_ratio, "moving points", m.moving_points, "moved", m.moved)
f2:release()

camera.close()
```

The same captured raw frame can be displayed and analyzed before it is released:

```lua
local frame = camera.get_frame(3000)
local rgb565 <close> = image.convert(frame, image.RGB565)
display.draw_image(0, 0, rgb565, {
    mode = "fit",
    width = display.width,
    height = display.height,
})
local result = motion.detect(frame, { stride = 8, pixel_threshold = 0.04, moving_threshold = 0.03 })
frame:release()
```

ESPDet with a JPEG file:

```lua
local espdet = require("espdet")
local image = require("image")
local storage = require("storage")

local root = storage.get_root_dir()
local model_path = storage.join_path(root, "test", "espdet_pico_224_224_cat.espdl")
local image_path = storage.join_path(root, "test", "cat.jpg")

espdet.load(model_path, { score_threshold = 0.6 })

local source <close> = image.load_file(image_path)
local result = espdet.detect(source, { score_threshold = 0.6 })

print("detection count=" .. tostring(result.count))
espdet.unload()
```

## Notes

- Supported source frame formats for motion: `RGB3`, `BGR3`, `GREY`, `Y800`, `RGBP`, `RGBR`, `YUYV`, `UYVY`, `JPEG`, and `MJPG`.
- Display preview should convert the camera frame through `image.convert(frame, image.RGB565)` and pass the RGB565 frame view to `display.draw_image()`. If the same source frame is also used for motion detection, `motion.detect(frame, opts)` can request GRAY8 and reuse the cached RGB565 path internally.
- `pixel_threshold` is a per-sample gray-value ratio threshold in `[0, 1]`; `0.04` is about a 10-level gray difference.
- `moving_threshold` is a moving sample ratio threshold in `[0, 1]`; `0.03` means more than 3% of sampled points must change.
- Detection results include `moving_points`, `sample_points`, and `moving_ratio = moving_points / sample_points`.
- `motion.detect(frame, opts)` compares `frame` with an internal copy of the previous frame and then updates that copy.
- `motion.detect(frame1, frame2, opts)` compares two explicit frames.
- `motion.reset()` clears the internal previous-frame copy.
- Import ESPDet with `local espdet = require("espdet")`.
- `espdet.detect(frame, opts)` accepts an `image.frame` and internally requests RGB565LE through the shared `image` module.
- Load a model once with `espdet.load(path[, opts])`, or pass `opts.model_path` on each `detect()` call.
- Raw byte input is interpreted as RGB565LE: `espdet.detect(data, width, height[, opts])`.
- Detection results include `count`; each detection includes `category`, `score`, `box`, `left`, `top`, `right`, `bottom`, `x`, `y`, `width`, and `height`.

## Console Test

```text
lua --run --path <DATA_ROOT>/scripts/builtin/test/espdet_image.lua --args-json "{\"image_path\":\"<DATA_ROOT>/test/cat.jpg\",\"model_path\":\"<DATA_ROOT>/test/espdet_pico_224_224_cat.espdl\",\"score_threshold\":0.6}" --timeout-ms 60000
```
