# lua_module_vision

Lua vision modules backed by `image.frame` buffers.

## Modules

- `motion_detect`: detects local motion in consecutive frames. It is enabled by default with `LUA_MODULE_VISION_MOTION_DETECT`.
- `color_detect`: detects the largest matching color blob. It is enabled by default with `LUA_MODULE_VISION_COLOR_DETECT`.
- `qrcode_detect`: finds and decodes QR codes. It is enabled by default with `LUA_MODULE_VISION_QRCODE_DETECT`.
- `espdet`: runs ESPDet object detection with a user-provided `.espdl` model. Enable it with `LUA_MODULE_VISION_ESPDET`.

All functions borrow frame data only during the call. Pass `image.frame` values directly unless the API explicitly documents raw byte input.

## Motion Detection

`motion_detect` compares each frame with the previous frame and reports whether motion is active.

```lua
local camera = require("camera")
local motion = require("motion_detect")

local detector = motion.new({
    roi = { x = 0, y = 40, width = 240, height = 180 },
    pixel_diff_threshold = 24,
    active_pixel_percent = 5,
    confirm_frames = 2,
    hold_frames = 3,
})

local frame <close> = camera.get_frame(3000)
local result = detector:detect(frame)

if result.motion and result.box then
    print("motion box", result.box.left, result.box.top,
          result.box.right, result.box.bottom)
end
```

The first `detect()` call seeds the previous-frame buffer and returns `ready = false`; subsequent calls compare against the preceding frame.

### API

- `motion.new([opts]) -> detector`: creates an independent detector.
- `detector:detect(frame) -> result`: runs detection with the detector's fixed configuration.
- `detector:reset()`: clears previous-frame and alert state.
- `detector:close()`: releases working buffers early. Garbage collection also releases them.

### Options

- `roi`: `{ x, y, width, height }`, defaulting to the whole frame.
- `pixel_diff_threshold`: luma-difference threshold, default `24`.
- `active_pixel_percent`: active ROI percentage required for raw detection, default `5`.
- `confirm_frames`: consecutive positive frames required to activate, default `2`.
- `hold_frames`: frames to hold an alert after raw detection clears, default `3`.
- `block_size`: motion-block edge length, default `4`.
- `block_hit_pixels`: changed pixels required to activate a block, default `12`.
- `box_padding`: padding around the raw motion box, default `2`.
- `box_deadband`: smoothed-edge changes to ignore, default `2`.
- `box_snap_threshold`: edge distance that snaps immediately, default `24`.

Results include `ready`, `motion`, `event`, `score`, and the smoothed display `box` when motion is active. Events are `"none"`, `"started"`, or `"stopped"`.

## Color Detection

```lua
local camera = require("camera")
local color_detect = require("color_detect")

local frame <close> = camera.get_frame(3000)
local result = color_detect.detect(frame, {
    source = { x = 0, y = 0, width = 240, height = 240 },
    h_min = 50,
    h_max = 88,
    s_min = 80,
    s_max = 255,
    v_min = 50,
    v_max = 255,
    min_pixels = 250,
    max_blob_pixels = 20000,
})

if result.detected then
    print("color center", result.cx, result.cy)
end
```

`color_detect.detect(frame[, opts])` accepts an `image.frame` and returns a table. Options include `source = { x, y, width, height }`, `h_min`, `h_max`, `s_min`, `s_max`, `v_min`, `v_max`, `l_min`, `l_max`, `a_min`, `a_max`, `b_min`, `b_max`, `x_stride`, `y_stride`, `min_pixels`, `max_blob_pixels`, and `max_pixels`. `s_*` and `v_*` may use `0..1` or `0..255`; hue uses `0..180`. Detection runs on a resized image of at most `100x100`; results are mapped back to the source image. Both strides default to `2` and accept `1..16`.

Provide all six LAB fields to use a direct LAB threshold (`L: 0..100`, `A/B: -128..127`); LAB takes precedence over HSV fields. If no LAB field is present, the hue range is split into LAB thresholds internally; partial LAB input is rejected. Converted thresholds are cached until color parameters change.

```lua
local result = color_detect.detect(frame, {
    l_min = 20, l_max = 80,
    a_min = -20, a_max = 30,
    b_min = 10, b_max = 70,
    x_stride = 2, y_stride = 2,
    min_pixels = 250,
})
```

Results include `count`, `detected`, `width`, `height`, and `source_*` fields. When a blob is detected, the table also includes `pixels`, `category`, `score`, `box`, `left`, `top`, `right`, `bottom`, `x`, `y`, `box_width`, `box_height`, `cx`, and `cy`.

`color_detect.release()` releases detector resources early.

## QR Code Detection

```lua
local qrcode = require("qrcode_detect")

local result = qrcode.detect(frame, {
    roi = { x = 0, y = 0, width = 640, height = 480 },
})

for i = 1, result.count do
    print(result[i].payload, result[i].left, result[i].top)
end
```

`qrcode_detect.detect(frame[, opts])` accepts any `image.frame` convertible to grayscale. `opts.roi` optionally limits detection to `{x, y, width, height}`. Up to four codes and 255 payload bytes per code are returned.

The result is an array with `count`, `total`, and `truncated` fields. Each code contains `payload`, `payload_len`, `version`, `ecc_level`, `mask`, `data_type`, `eci`, `left`, `top`, `right`, `bottom`, `x`, `y`, `width`, `height`, and four `{x, y}` corners. `truncated` is true when additional codes or payload bytes did not fit.

## ESPDet

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

API:
- `espdet.load(path[, opts])` loads a model and returns `true`.
- `espdet.detect(frame[, opts])` accepts an `image.frame`.
- `espdet.detect(rgb565_bytes, width, height[, opts])` accepts raw RGB565LE bytes.
- `espdet.unload()` releases the loaded model and returns no values.

Options include `model_path` or `path`, `model_name`, `score_threshold` or `score_thr`, and `nms_threshold` or `nms_thr`. Results include `count`; each detection includes `category`, `score`, `box`, `left`, `top`, `right`, `bottom`, `x`, `y`, `width`, `height`, and optional `keypoint`.
