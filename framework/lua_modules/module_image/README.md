# Lua Image

Shared image type and conversion helpers for Lua. Every frame produced by
`camera` (and any future producer such as JPEG / file / network loaders) is an
`image.frame` userdata defined by this module, so consumers like
`display` and `vision` only need to learn one type.

## How to call
- `local image = require("image")`
- `image.convert(frame, format)` ensures the requested `image.*` format exists
  in the frame's shared store and returns a new `image.frame` view for that format.
- `image.resize(frame, opts)` returns a new, independent `image.frame` scaled
  to `opts.width` x `opts.height`. See "Example: resize" below.
- `image.load_file(path)` reads an image file and returns an `image.frame`.
- `image.save_file(path, frame)` saves a frame using the format implied by the
  file suffix.

## Format constants

Use these constants with `image.convert(frame, format)`:

| Constant | Output format |
|---|---|
| `image.RGB565` | RGB565 little-endian |
| `image.RGB565_BE` | RGB565 big-endian |
| `image.RGB888` | RGB888 |
| `image.BGR888` | BGR888 |
| `image.GRAY8` | 8-bit grayscale |
| `image.YUYV` | YUV 4:2:2 packed |
| `image.UYVY` | YUV 4:2:2 packed (swapped) |
| `image.JPEG` | JPEG still |
| `image.MJPEG` | Motion-JPEG frame |

## Frame type: `image.frame`

An `image.frame` is a Lua-visible format view over a shared image store. The
store owns the original buffer and any cached converted buffers. Methods:

- `frame:info()` returns `{ width, height, bytes, pixel_format, timestamp_us, valid }`
- `frame:data()` copies the buffer into a Lua string (slow, allocates)
- `frame:release()` releases this view; the store is freed when the last view is released

Release happens automatically when the variable is declared with the Lua 5.4+
`<close>` attribute or when it is collected by GC, but explicit `<close>` is the
recommended style:

```lua
do
    local frame <close> = camera.get_frame(1000)
    -- ... use frame ...
end
-- frame is already released here
```

Converted frames share the same store as their source. Releasing the source
frame does not invalidate converted views that are still alive. For V4L2 camera
frames, the producer buffer is returned only when the last view for that frame
is released, so release all frame views promptly so capture can continue.

## Pixel format names

`frame:info().pixel_format` is a 4-character FOURCC string. The image module
understands these tokens:

| Token | Meaning |
|---|---|
| `RGBP` | RGB565 little-endian |
| `RGBR` | RGB565 big-endian |
| `RGB3` | RGB888 |
| `BGR3` | BGR888 |
| `GREY` / `Y800` | 8-bit grayscale |
| `YUYV` | YUV 4:2:2 packed |
| `UYVY` | YUV 4:2:2 packed (swapped) |
| `JPEG` | JPEG still |
| `MJPG` | Motion-JPEG frame |

Consumers internally request the format they need (e.g. `display` asks for
`RGBP`; `vision` asks for `GREY`); scripts pass the frame object directly and
never need to select a conversion path manually.

## Example: convert a frame

```lua
local image = require("image")

do
    local gray <close> = image.convert(frame, image.GRAY8)
    local jpeg <close> = image.convert(frame, image.JPEG)
end
```

The converted result is a new `image.frame` view backed by the same shared
store. Repeated conversions reuse cached buffers when possible. Release views
with `<close>`, `frame:release()`, or GC.

## Example: resize

`image.resize(frame, opts)` returns a new, independent `image.frame` at
`opts.width` x `opts.height`. Optional `opts.format` selects the output
(`image.RGB565` or `image.GRAY8` only; defaults to RGB565, or GRAY8 when the
source is already gray). Optional `opts.filter` is `"nearest"` (default) or
`"bilinear"`. Output dimensions follow the same 1920 x 1080 pixel cap as
conversion.

```lua
local image = require("image")

do
    local small <close> = image.resize(frame, { width = 96, height = 96 })

    local probe <close> = image.resize(frame, {
        width  = 64,
        height = 64,
        format = image.GRAY8,
        filter = "bilinear",
    })

    local thumb <close> = image.resize(frame, { width = 160, height = 120 })
    local jpeg  <close> = image.convert(thumb, image.JPEG)
    image.save_file("/sdcard/thumb.jpg", jpeg)
end
```

## Example: load JPEG from disk

```lua
local display = require("display")
local image   = require("image")
local storage = require("storage")

do
    local frame <close> = image.load_file(storage.join_path(storage.get_root_dir(), "picture.jpg"))
    local rgb565 <close> = image.convert(frame, image.RGB565)
    display.draw_image(0, 0, rgb565, {
        mode = "fit",
        width = display.width,
        height = display.height,
    })
    image.save_file(storage.join_path(storage.get_root_dir(), "copy.jpg"), frame)
end
```

`load_file()` and `save_file()` currently support `.jpg` / `.jpeg`. The returned
frame keeps the file bytes alive until `frame:release()`, `<close>`, or GC
releases it. The frame metadata reports the JPEG width, height, byte size, and
`pixel_format = "JPEG"`.

## Resource limits

This module is designed for MCU-class devices and rejects oversized images
before conversion:

- JPEG files loaded from disk are limited to 4 MiB.
- Decoded or converted frames are limited to 1920 x 1080 pixels.
- Conversion may allocate cached output buffers in PSRAM; JPEG encoding may
  also allocate a compressed output buffer and only creates an aligned input
  copy when the source buffer is not already 16-byte aligned.

Use camera resolutions and file sizes that fit the available PSRAM budget, and
release all `image.frame` views promptly. Cached buffers are frame-local and
are released when the last view for that frame is released.

## Example: snapshot to disk

```lua
local camera         = require("camera")
local image          = require("image")
local storage        = require("storage")

do
    local frame <close> = camera.get_frame(3000)
    image.save_file(storage.join_path(storage.get_root_dir(), "snapshot.jpg"), frame)
end
```

## C-side use (for module authors)

Other Lua modules can read or produce frames without going through Lua method
dispatch by linking against this component and using:

- `lua_image_push_frame(L, data, bytes, &info, release_cb, ctx)` — wrap
  a producer-owned buffer as an `image.frame` userdata. `release_cb`
  is called exactly once when the frame is released (via `frame:release()`,
  `<close>`, or `__gc`). On failure the function returns an error code and
  does not invoke `release_cb`; the caller still owns the buffer.
- `lua_image_borrow_frame(L, index, &out)` — read `data`, `bytes`, and
  `info` from the selected frame view without copying. Valid only for the
  duration of the C call; do not retain the pointer after returning to Lua.
- `lua_image_require_format(L, index, fmt, &view)` — get the
  frame in a specific format, possibly converting and caching in the shared
  store. Pair with `lua_image_release_view(&view)` and do not retain `view.data`
  after returning to Lua.
