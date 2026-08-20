# Lua Camera

The `camera` module discovers capture devices, opens one capture stream, and
returns captured frames as `image.frame` objects.

```lua
local camera = require("camera")
```

## Error handling and lifetime

- Invalid arguments and camera operation failures raise Lua errors. Use
  `pcall()` when the caller needs to handle an error.
- The module manages one active camera stream. Open the camera before calling
  `info()`, `list_formats()`, `get_frame()`, `flush()`, `set_vflip()`, or
  `set_hmirror()`.
- A frame returned by `get_frame()` must be released before `camera.close()`.
  The recommended pattern is a Lua 5.4+ `<close>` variable.

```lua
do
    local frame <close> = camera.get_frame(3000)
    -- Use frame here.
end
-- The frame has been released.
```

## Discover devices

### `camera.list_devices()`

Returns an array of currently available capture devices:

```lua
{
  {
    path = string,
    source = "mipi_csi" | "dvp" | "spi" | "usb_uvc" | "unknown",
    removable = boolean,
    capabilities = integer,
  },
  -- ...
}
```

Pass a device's `path` to `camera.open()`. `capabilities` is a device-provided
bitmask; this module does not provide constants for decoding it.

## Open and close

### `camera.open(dev_path [, opts])`

Opens `dev_path` and starts capture. Returns `true` on success.

When called without options while a stream is already open, `camera.open()` is
idempotent and returns `true`. To change only an exact width or height, close
the active stream before calling `open()` again. Requests with `format`, or
with both dimensions and `nearest = true`, can replace the active stream; they
fail if any frame is still held.

`opts` is optional and supports:

- `width`: integer from 0 to 4294967295. Omit or use `0` to keep the device
  default.
- `height`: integer from 0 to 4294967295. Omit or use `0` to keep the device
  default.
- `format`: a non-empty preference array of up to 16 four-character FOURCC
  strings. The first format offered by the device is selected. For example:
  `{ "JPEG", "RGBP", "YUYV" }`.
- `nearest`: boolean. When `true` and both `width` and `height` are non-zero,
  selects the closest advertised size for the selected format when available.

The device may adjust a requested width, height, or format. Always call
`camera.info()` after opening to obtain the active stream configuration.

### `camera.close()`

Closes the active stream and returns `true`. It raises an error until every
frame and every view derived from a frame has been released.

## Inspect the active stream

### `camera.info()`

Returns:

```lua
{
  width = integer,
  height = integer,
  pixel_format = string, -- four-character FOURCC
}
```

### `camera.is_open()`

Returns whether a camera stream is open.

### `camera.is_streaming()`

Returns whether the open camera stream is currently capturing.

## Discover supported formats

### `camera.list_formats()`

Returns an array describing formats available from the active camera:

```lua
{
  {
    format = string,       -- four-character FOURCC
    description = string,
    sizes = {
      { w = integer, h = integer, fps = { number, ... } }, -- fps is optional
      -- ...
    },
  },
  -- ...
}
```

Only discrete sizes and frame rates are listed. If the device reports a size
range rather than discrete sizes, `sizes` is an empty array for that format.

## Capture frames

### `camera.get_frame([timeout_ms])`

Captures and returns an `image.frame`. `timeout_ms` must be a non-negative
integer; zero or omission uses a 5000 ms timeout.

The returned object follows the `image.frame` API:

- `frame:info()` returns `{ width, height, bytes, pixel_format, timestamp_us, valid }`.
- `frame:data()` returns a copy of the frame bytes as a Lua string.
- `frame:release()` releases the frame. It is also released by `<close>` or
  garbage collection.

Release every frame promptly. Holding frames can prevent later captures,
`camera.flush()`, and `camera.close()`.

### `camera.flush()`

Discards frames waiting to be read and returns `true`. It requires an active
stream with no currently held frames.

## Orientation

### `camera.set_vflip(enabled)`

Sets vertical flip on the active camera and returns `true`. `enabled` must be
a boolean.

### `camera.set_hmirror(enabled)`

Sets horizontal mirror on the active camera and returns `true`. `enabled` must
be a boolean.

The camera must support the requested orientation control.

## Example: capture and save a JPEG

```lua
local camera = require("camera")
local image = require("image")
local storage = require("storage")

local devices = camera.list_devices()
assert(#devices > 0, "no camera available")
assert(camera.open(devices[1].path, {
    width = 640,
    height = 480,
    format = { "JPEG", "RGBP" },
    nearest = true,
}))

do
    local frame <close> = camera.get_frame(3000)
    image.save_file(storage.join_path(storage.get_root_dir(), "capture.jpg"), frame)
end

assert(camera.close())
```
