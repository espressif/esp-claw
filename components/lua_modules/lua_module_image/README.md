# Lua Image

Shared image type and conversion helpers for Lua. Frames produced by `camera` or `image.load_file()` are `image.frame` userdata values that can be passed to `display` and `vision`.

## How to call
- `local image = require("image")`
- `image.convert(frame, format)` ensures the requested `image.*` format exists
  in the frame's shared store and returns a new `image.frame` view for that format.
- `image.resize(frame, opts)` returns a new, independent `image.frame` scaled
  to `opts.width` x `opts.height`. See "Example: resize" below.
- `image.load_file(path)` reads an image file and returns an `image.frame`.
- `image.save_file(path, frame)` saves a frame using the format implied by the
  file suffix. It returns no values on success.
- Invalid arguments and image I/O failures raise Lua errors.

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

An `image.frame` is a Lua-visible image view. Methods:

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

Releasing the source frame does not invalidate converted views that are still alive. For camera frames, the capture buffer is returned only when the last view for that frame is released, so release all frame views promptly so capture can continue.

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

Consumers request the format they need; scripts usually pass the frame object directly.

## Example: convert a frame

```lua
local image = require("image")

do
    local gray <close> = image.convert(frame, image.GRAY8)
    local jpeg <close> = image.convert(frame, image.JPEG)
end
```

The converted result is a new `image.frame` view. Release views with `<close>`, `frame:release()`, or GC.

## Example: resize

`image.resize(frame, opts)` returns a new, independent `image.frame` at
`opts.width` x `opts.height`. Optional `opts.format` selects the output
(`image.RGB565` or `image.GRAY8` only; defaults to RGB565, or GRAY8 when the
source is already gray). Optional `opts.filter` is `"nearest"` (default) or
`"bilinear"`. Output dimensions follow the same 1920 x 1080 pixel cap as
conversion.

```lua
local image = require("image")
local storage = require("storage")

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
    image.save_file(storage.join_path(storage.get_root_dir(), "thumb.jpg"), jpeg)
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
- Conversion and JPEG encoding may allocate additional buffers.

Use camera resolutions and file sizes that fit the available memory budget, and release all `image.frame` views promptly.

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
