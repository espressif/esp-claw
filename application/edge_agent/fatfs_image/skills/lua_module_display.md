# Lua Display

This skill describes how to correctly use `display` when writing Lua scripts.

`display` is a low-level drawing module. It can:
- Initialize and deinitialize the LCD drawing context
- Draw text, lines, rectangles, circles, arcs, ellipses, triangles, and round rectangles
- Draw raw RGB565 bitmaps
- Draw JPEG and PNG images from memory or files
- Manage frame-based rendering and partial screen flushes

## Typical setup

In this project, `display` is usually used together with `board_manager`:

```lua
local board_manager = require("board_manager")
local display = require("display")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

display.init(panel_handle, io_handle, width, height, panel_if)
```

After `display.init(...)` succeeds:
- `display.width()` returns the current screen width
- `display.height()` returns the current screen height
- Most drawing APIs can be used
- The display arbiter automatically grants Lua foreground ownership for the lifetime of the display session

When finished:

```lua
pcall(display.end_frame)
pcall(display.deinit)
```

## Important rules

- All coordinates and sizes are integer arguments unless noted otherwise.
- Most numeric drawing arguments are validated as integers in the Lua binding.
- Passing floating-point values such as `10.5`, `32.2`, or `tilt / 2` to coordinates, widths, heights, radii, crop rectangles, flush rectangles, or `font_size` can raise a Lua error instead of being rounded automatically.
- If a computed value is meant to be a pixel coordinate or size, convert it to an integer first before passing it to the display API. Prefer integer division `//` when the value comes from a division expression.
- Colors are almost always passed as three integers: `r, g, b`.
- Text drawing only supports ASCII text.
- For Chinese or other Unicode text, render an image and draw it with `draw_png_file(...)` or `draw_jpeg_file(...)`.
- Image file paths must be absolute paths under the current storage root, for example `storage.join_path(storage.get_root_dir(), "pic.jpg")`.
- Paths containing `..` are rejected.
- Supported image file extensions are `.jpg`, `.jpeg`, and `.png`.
- This is critical: screen display duration must be considered. Do not deinitialize or exit immediately after `present()`, or the image may only flash briefly. Keep the display session alive long enough, and handle that hold time asynchronously when appropriate.

## Screen lifecycle

### `display.init(panel_handle, io_handle, lcd_width, lcd_height[, panel_if])`

Initializes the drawing context.

- `panel_handle`: lightuserdata, usually from `board_manager.get_display_lcd_params(...)`
- `io_handle`: lightuserdata or `nil`
- `lcd_width`: integer
- `lcd_height`: integer
- `panel_if`: optional interface constant, usually returned by `board_manager.get_display_lcd_params(...)`
- Common values come from `board_manager.PANEL_IF_IO`, `board_manager.PANEL_IF_RGB`, and `board_manager.PANEL_IF_MIPI_DSI`
- Returns `true` on success
- Raises a Lua error on failure

### `display.deinit()`

Deinitializes the drawing context.

- Returns `true` on success
- Raises a Lua error on failure

### `display.width()`

Returns the current screen width.

### `display.height()`

Returns the current screen height.

## Frame rendering

The module supports frame-based rendering. This is the preferred mode when a script draws a full screen or updates multiple primitives together.

### `display.begin_frame([options])`

Starts a frame.

`options` is an optional table:
- `clear`: boolean, default `true`
- `r`: background red, default `0`
- `g`: background green, default `0`
- `b`: background blue, default `0`

Example:

```lua
display.begin_frame({ clear = true, r = 12, g = 18, b = 28 })
```

### `display.present()`

Flushes the full current frame to the panel.

### `display.present_rect(x, y, width, height)`

Flushes only a rectangular region. This is useful for partial updates such as painting apps or small widgets.

### `display.end_frame()`

Ends the current frame.

### `display.frame_active()`

Returns a boolean indicating whether a frame is currently active.

### `display.animation_info()`

Returns a table with runtime rendering information:
- `framebuffer_count`
- `double_buffered`
- `frame_active`
- `flush_in_flight`

## Backlight

### `display.backlight(on)`

Turns the display backlight on or off.

- `on`: boolean

Example:

```lua
display.backlight(true)
```

## Text APIs

### `display.draw_text(x, y, text [, options])`

Draws ASCII text at the given position.

`options` is an optional table:
- `r`, `g`, `b`: text color, default white
- `font_size`: integer, default `24`; floating-point values are rejected
- `bg_r`, `bg_g`, `bg_b`: optional background color; if any background field is given, background fill is enabled

Example:

```lua
display.draw_text(16, 24, "hello", {
    r = 255,
    g = 255,
    b = 255,
    font_size = 24,
})
```

Restrictions:
- `text` must be ASCII
- Non-ASCII text raises an error

### `display.measure_text(text [, options])`

Measures text without drawing it.

`options` currently supports:
- `font_size` as an integer

Returns:
- `width`
- `height`

Example:

```lua
local tw, th = display.measure_text("hello", { font_size = 24 })
```

### `display.draw_text_aligned(x, y, width, height, text [, options])`

Draws ASCII text inside a rectangle with alignment.

`options` supports:
- `r`, `g`, `b`
- `font_size` as an integer
- `bg_r`, `bg_g`, `bg_b`
- `align`: `"left"`, `"center"`/`"centre"`, or `"right"`
- `valign`: `"top"`, `"middle"`/`"center"`, or `"bottom"`

Example:

```lua
display.draw_text_aligned(0, 0, display.width(), 32, "status", {
    r = 255,
    g = 255,
    b = 255,
    font_size = 16,
    align = "center",
    valign = "middle",
})
```

## Basic drawing primitives

### `display.clear(r, g, b)`

Clears the screen or current frame buffer to a solid color.

### `display.set_clip_rect(x, y, width, height)`

Sets a clipping rectangle. Subsequent drawing is restricted to that region until cleared.

### `display.clear_clip_rect()`

Removes the active clipping rectangle.

### `display.fill_rect(x, y, width, height, r, g, b)`

Draws a filled rectangle.

### `display.draw_rect(x, y, width, height, r, g, b)`

Draws a rectangle outline.

### `display.draw_pixel(x, y, r, g, b)`

Draws one pixel.

### `display.draw_line(x0, y0, x1, y1, r, g, b)`

Draws a line.

## Shape drawing

### `display.fill_circle(cx, cy, radius, r, g, b)`

Draws a filled circle.

### `display.draw_circle(cx, cy, radius, r, g, b)`

Draws a circle outline.

### `display.draw_arc(cx, cy, radius, start_deg, end_deg, r, g, b)`

Draws an arc.

- `start_deg` and `end_deg` are numeric values, not limited to integers

### `display.fill_arc(cx, cy, inner_radius, outer_radius, start_deg, end_deg, r, g, b)`

Draws a filled ring segment.

### `display.draw_ellipse(cx, cy, radius_x, radius_y, r, g, b)`

Draws an ellipse outline.

### `display.fill_ellipse(cx, cy, radius_x, radius_y, r, g, b)`

Draws a filled ellipse.

### `display.draw_round_rect(x, y, width, height, radius, r, g, b)`

Draws a rounded rectangle outline.

### `display.fill_round_rect(x, y, width, height, radius, r, g, b)`

Draws a filled rounded rectangle.

### `display.draw_triangle(x1, y1, x2, y2, x3, y3, r, g, b)`

Draws a triangle outline.

### `display.fill_triangle(x1, y1, x2, y2, x3, y3, r, g, b)`

Draws a filled triangle.

## Raw bitmap APIs

These APIs draw RGB565 pixel buffers.

### `display.draw_bitmap(x, y, w, h, data)`

Draws a raw RGB565 bitmap.

- `data` is either a Lua string containing exactly `w * h * 2` bytes or more, or a `lightuserdata` pointer to a buffer of that size
- Byte order must be RGB565 little-endian

If the byte string is shorter than required, the API raises an error.

### `display.draw_bitmap_crop(x, y, src_x, src_y, width, height, src_width, src_height, data)`

Draws a cropped region from a larger RGB565 source image.

Arguments:
- `x`, `y`: destination position on screen
- `src_x`, `src_y`: top-left coordinate inside the source image
- `width`, `height`: crop size to draw
- `src_width`, `src_height`: full source image size
- `data`: source RGB565 data buffer
- `data` may be a Lua string or `lightuserdata`

Notes:
- `data` must contain the full source image, not just the crop
- Required byte size is `src_width * src_height * 2`
- Input data is RGB565 little-endian

### `display.draw_rgb565_crop(x, y, src_x, src_y, width, height, src_width, src_height, data)`

Alias-style crop API for in-memory RGB565 data, matching the JPEG crop naming pattern.

- Returns `width, height`

### `display.draw_rgb565_scaled(x, y, src_width, src_height, scale_w, scale_h, data)`

Scales an in-memory RGB565 image to the requested output size.

- `data` may be a Lua string or `lightuserdata`
- Returns `output_w, output_h`

### `display.draw_rgb565_fit(x, y, src_width, src_height, max_w, max_h, data)`

Scales an in-memory RGB565 image to fit within `max_w x max_h` while preserving aspect ratio.

- If the original image already fits, it is drawn at full resolution
- Otherwise the module computes a scaled size automatically
- For larger outputs, the chosen scaled size is aligned down to multiples of `8` when possible
- `data` may be a Lua string or `lightuserdata`
- Returns `output_w, output_h`

## JPEG and PNG APIs

### `display.draw_jpeg(x, y, jpeg_data)`

Draws a JPEG from raw bytes in memory.

- `jpeg_data` is a Lua string containing JPEG bytes
- Returns `width, height`

### `display.draw_jpeg_file(x, y, path)`

Draws a JPEG from a file.

- `path` must be an absolute `.jpg` or `.jpeg` path
- Returns `width, height`

### `display.draw_png_file(x, y, path)`

Draws a PNG from a file.

- `path` must be an absolute `.png` path
- Returns `width, height`

PNG handling details:
- The PNG is decoded inside the module
- RGBA PNG input is converted to RGB565
- Alpha is pre-multiplied against black

This means transparent PNG pixels are blended toward black before display, not against an arbitrary background.

### `display.draw_jpeg_crop(x, y, src_x, src_y, width, height, jpeg_data)`

Draws a cropped region from in-memory JPEG data.

- Returns `width, height`

### `display.draw_jpeg_file_crop(x, y, src_x, src_y, width, height, path)`

Draws a cropped region from a JPEG file.

- Returns `width, height`

### `display.draw_jpeg_file_scaled(x, y, scale_w, scale_h, path)`

Draws a JPEG file scaled to the requested output size.

Restrictions from the implementation:
- `scale_w` and `scale_h` must be positive
- `scale_w` and `scale_h` must both be multiples of `8`

Returns:
- `output_w`
- `output_h`

### `display.draw_jpeg_file_fit(x, y, max_w, max_h, path)`

Scales a JPEG to fit within `max_w x max_h` while preserving aspect ratio.

Behavior:
- If the original image already fits, it is drawn at full resolution
- Otherwise the module computes a scaled size automatically
- The chosen scaled size is aligned down to multiples of `8`
- Minimum output size is clamped to `8x8`
- Maximum downscale is limited to `1/8` of the original size

Returns:
- `output_w`
- `output_h`

## Error behavior and constraints

- Most APIs raise Lua errors directly when arguments are invalid or the HAL returns an error
- Integer-only APIs reject non-integer Lua values
- Path-based image APIs reject invalid or unsafe paths
- `draw_bitmap(...)` and `draw_bitmap_crop(...)` reject buffers that are too short
- `draw_text(...)` and `draw_text_aligned(...)` reject non-ASCII text

## Recommended usage pattern

For normal screen rendering:
1. Use `board_manager.get_display_lcd_params("display_lcd")`
2. Call `display.init(...)`
3. Call `display.begin_frame(...)`
4. Draw text, shapes, or images
5. Call `display.present()` or `display.present_rect(...)`
6. Call `display.end_frame()`
7. Call `display.deinit()` before exit

## Example

```lua
local bm = require("board_manager")
local display = require("display")

local panel_handle, io_handle, width, height, panel_if =
    bm.get_display_lcd_params("display_lcd")

display.init(panel_handle, io_handle, width, height, panel_if)

display.begin_frame({ clear = true, r = 12, g = 18, b = 28 })

display.draw_rect(12, 12, display.width() - 24, display.height() - 24, 80, 120, 160)
display.fill_rect(20, 40, 80, 36, 72, 208, 235)
display.draw_text(24, 90, "Lua Display Demo", {
    r = 245,
    g = 244,
    b = 238,
    font_size = 24,
})
display.draw_text_aligned(0, display.height() - 24, display.width(), 20, "frame api", {
    r = 210,
    g = 220,
    b = 228,
    font_size = 16,
    align = "center",
    valign = "middle",
})

display.present()
display.end_frame()
display.deinit()
```
