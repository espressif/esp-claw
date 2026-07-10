# Lua Camera

Minimal Lua bindings for the V4L2 camera service. The module exposes one
borrow-and-release frame API and intentionally leaves format conversion and
filesystem I/O to other modules.

## How to call
- `local camera = require("camera")`
- `camera.open(dev_path [, opts])` before any other call (opts is optional, see below)
- `camera.info()` returns `{ width, height, pixel_format }` for the active stream
- `camera.get_frame([timeout_ms])` borrows one frame; release frames when they are no longer needed
- `camera.flush()` drops every queued buffer so the next `get_frame()` returns a fresh capture
- `camera.is_open()` / `camera.is_streaming()` query state without raising
- `camera.list_formats()` reports what the sensor/driver can negotiate
- `camera.close()` when the camera is no longer needed

## Negotiating resolution / pixel format

Pass `opts` to ask the driver for a specific capture configuration. Any field
that is `nil` keeps the driver default. `camera.info()` always reflects what
was actually accepted.

```lua
camera.open(paths.dev_path, {
    width   = 640,
    height  = 480,
    format  = { "JPEG", "RGBP" }, -- priority list; first one the sensor speaks wins
    nearest = true,               -- snap width/height to the closest supported size
})
local stream = camera.info()
print(stream.width, stream.height, stream.pixel_format)
```

### `opts.format`

Array of FOURCC strings ordered by preference. Valid tokens: `RGBP`, `RGBR`,
`RGB3`, `BGR3`, `YUYV`, `UYVY`, `GREY` / `Y800`, `YU12`, `JPEG`, `MJPG`.

- `{ "RGBP" }` — strictly this format. Errors with the list of advertised
  formats if the sensor does not support it.
- `{ "JPEG", "RGBP", "YUYV" }` — try in order; the first FOURCC the sensor
  actually advertises wins.
- omitted — keep the driver default.

### `opts.width` / `opts.height`

Requested capture size. Omitted means "use the first size the chosen format
advertises". May be combined with `format` and `nearest`.

### `opts.nearest`

When `true`, the requested `width`/`height` is snapped to the closest size the
chosen format actually supports before the driver sees the request. Composes
with `format`: the FOURCC is picked first, then the nearest size lookup runs
under that FOURCC. Default `false`, which sends the exact requested size and
returns an error when the driver rejects it.

When the camera is already open and the call requests a reconfiguration (new
format or nearest snap), the session is closed and reopened transparently.

## Discovering what the sensor supports

`camera.list_formats()` returns the minimum information scripts usually need:

```lua
{
    format      = "JPEG",        -- 4-char FOURCC
    description = "Motion-JPEG", -- driver text
    sizes = {                    -- array of discrete sizes (may be empty)
        { w = 1600, h = 1200, fps = { 30, 15 } },  -- fps is optional (only when driver enumerates)
        { w = 640,  h = 480 },
    },
}
```

Walking it:

```lua
for _, f in ipairs(camera.list_formats()) do
    print(string.format("%s  (%s)", f.format, f.description))
    for _, s in ipairs(f.sizes) do
        local fps = s.fps and ("  fps=" .. table.concat(s.fps, ",")) or ""
        print(string.format("  %dx%d%s", s.w, s.h, fps))
    end
end
```

If a driver does not implement `VIDIOC_ENUM_FRAMESIZES` (some JPEG-only
sensors in `esp_video` fall in this bucket), or only reports stepwise /
continuous ranges, `sizes` simply comes back empty. Use `camera.info()` to read
what the active stream is producing, and `camera.open(opts)` with explicit
`{ width, height, format }` to probe what other configurations the driver
actually accepts — that is the only reliable test on drivers that won't
enumerate discrete sizes.

## Flushing stale frames

After long idle or sensor wake-up, the V4L2 queue may already hold buffers
captured before exposure/white balance stabilized. Drop them with:

```lua
camera.flush()                -- discard everything currently queued
local frame <close> = camera.get_frame(1000)
```

`flush()` is rejected with an error if one or more frames are still borrowed.

## Frame lifecycle

`camera.get_frame()` returns an `image.frame` userdata. The type and
its methods live in the `image` module so that any frame producer
(camera, future JPEG/file loaders, network streams) shares one contract:

- `frame:info()` returns `{ width, height, bytes, pixel_format, timestamp_us, valid }`
- `frame:data()` copies the buffer into a Lua string (slow, allocates)
- `frame:release()` returns the buffer to its producer

Important:
- `camera.get_frame()` is a **borrow** API, not a copy. The driver owns the buffer.
- Multiple frames may be borrowed at the same time, up to the camera driver buffer count. Release frames promptly so capture buffers return to the driver.
- Converted `image.frame` views can share the same borrowed camera buffer. Release all derived views before `camera.close()`.
- Prefer the Lua 5.4+ `<close>` attribute so the frame is released
  deterministically when the variable leaves scope:
  ```lua
  do
      local frame <close> = camera.get_frame(1000)
      -- use frame here; auto-released on scope exit
  end
  ```
- The raw format can be RGB565, YUV, GRAY, JPEG or MJPEG depending on the
  driver. Consumers (display, vision, JPEG encode) request the format they
  need through `image`.
- `camera.close()` fails with an explicit error when frames are still borrowed.

## Saving a frame as JPEG

Camera no longer owns JPEG encoding or filesystem I/O. Compose three small
modules instead:

```lua
local camera         = require("camera")
local image = require("image")
local storage        = require("storage")

do
    local frame <close> = camera.get_frame(3000)
    image.save_file(storage.join_path(storage.get_root_dir(), "capture.jpg"), frame)
end
```

## Example
```lua
local camera         = require("camera")
local board_manager  = require("board_manager")

local paths = board_manager.get_camera_paths()
camera.open(paths.dev_path)

local stream = camera.info()
print(stream.width, stream.height, stream.pixel_format)

do
    local frame <close> = camera.get_frame(1000)
    local info = frame:info()
    print(info.width, info.height, info.pixel_format, info.bytes)
end

camera.close()
```
