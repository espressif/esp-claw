# Lua Display

This module describes how to correctly use `display` when writing Lua scripts.

`display` is a low-level drawing module. It can:
- Initialize and deinitialize the LCD drawing context
- Draw text, lines, rectangles, circles, arcs, ellipses, triangles, and round rectangles
- Draw raw RGB565 pixel buffers
- Draw RGB565 buffers obtained from `image.frame` values through the `image` module
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
- `display.width` returns the current screen width
- `display.height` returns the current screen height
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
- Passing floating-point values such as `10.5`, `32.2`, or `tilt / 2` to coordinates, widths, heights, radii, crop rectangles, or `font_size` can raise a Lua error instead of being rounded automatically.
- If a computed value is meant to be a pixel coordinate or size, convert it to an integer first before passing it to the display API. Prefer integer division `//` when the value comes from a division expression.
- Colors are passed as one value: a hex string, a named color string, or a `{ r, g, b [, a] }` table.
- Supported hex forms are `#rgb`, `#rgba`, `#rrggbb`, and `#rrggbbaa`.
- Supported named colors include `black`, `white`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, and `transparent`.
- Text drawing only supports ASCII text.
- For Chinese or other Unicode text, render or load an image through the `image` module, convert it to `image.RGB565`, then draw the RGB565 buffer.
- Image file loading, saving, decoding, and format conversion belong to the `image` module. `display` only draws raw RGB565 buffers.
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

### `display.width`

Returns the current screen width.

### `display.height`

Returns the current screen height.

## Frame rendering

The module supports frame-based rendering. This is the preferred mode when a script draws a full screen or updates multiple primitives together.

### `display.begin_frame([options])`

Starts a frame.

`options` is an optional table:
- `clear`: boolean, default `true`
- `color`: background color, default `"black"`

Example:

```lua
display.begin_frame({ clear = true, color = "#0c1220" })
```

### `display.present()`

Flushes the current dirty rectangle to the panel. If no drawing operation changed the framebuffer since the last present, this returns without refreshing.

### `display.present_full()`

Flushes the full current frame to the panel and clears the dirty state.

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
- `color`: text color, default `"white"`
- `font_size`: integer, default `24`; floating-point values are rejected
- `bg`: optional background color
- Text color and background can include alpha when drawing inside an active frame.

Example:

```lua
display.draw_text(16, 24, "hello", {
    color = "white",
    font_size = 24,
})
```

Restrictions:
- `text` must be ASCII
- Non-ASCII text raises an error
- Semi-transparent text or background requires `begin_frame(...)` before drawing.

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
- `color`
- `font_size` as an integer
- `bg`
- `align`: `"left"`, `"center"`/`"centre"`, or `"right"`
- `valign`: `"top"`, `"middle"`/`"center"`, or `"bottom"`

Example:

```lua
display.draw_text_aligned(0, 0, display.width, 32, "status", {
    color = "white",
    font_size = 16,
    align = "center",
    valign = "middle",
})
```

## Basic drawing primitives

### `display.clear(color)`

Clears the screen or current frame buffer to a solid color.

### `display.set_clip_rect(x, y, width, height)`

Sets a clipping rectangle. Subsequent drawing is restricted to that region until cleared.

### `display.clear_clip_rect()`

Removes the active clipping rectangle.

### `display.fill_rect(x, y, width, height, color)`

Draws a filled rectangle.

### `display.draw_rect(x, y, width, height, color)`

Draws a rectangle outline.

### `display.draw_pixel(x, y, color)`

Draws one pixel.

### `display.draw_line(x0, y0, x1, y1, color)`

Draws a line.

## Shape drawing

### `display.fill_circle(cx, cy, radius, color)`

Draws a filled circle.

### `display.draw_circle(cx, cy, radius, color)`

Draws a circle outline.

### `display.draw_arc(cx, cy, radius, start_deg, end_deg, color)`

Draws an arc.

- `start_deg` and `end_deg` are numeric values, not limited to integers

### `display.fill_arc(cx, cy, inner_radius, outer_radius, start_deg, end_deg, color)`

Draws a filled ring segment.

### `display.draw_ellipse(cx, cy, radius_x, radius_y, color)`

Draws an ellipse outline.

### `display.fill_ellipse(cx, cy, radius_x, radius_y, color)`

Draws a filled ellipse.

### `display.draw_round_rect(x, y, width, height, radius, color)`

Draws a rounded rectangle outline.

### `display.fill_round_rect(x, y, width, height, radius, color)`

Draws a filled rounded rectangle.

### `display.draw_triangle(x1, y1, x2, y2, x3, y3, color)`

Draws a triangle outline.

### `display.fill_triangle(x1, y1, x2, y2, x3, y3, color)`

Draws a filled triangle.

## Raw pixel APIs

These APIs draw RGB565 pixel buffers. Prefer `display.draw_image(...)` when the source is already an `image.frame`, because it borrows the image buffer during the C call and avoids creating a large Lua string.

### `display.draw_pixels(x, y, data, opts)`

Draws a raw RGB565 pixel buffer.

- `data` is either a Lua string containing at least `opts.width * opts.height * 2` bytes, or a `lightuserdata` pointer to a buffer of that size
- `opts` is required because raw buffers do not carry width or height metadata
- `opts.format`: `"rgb565"` or `"rgb565le"`; default is `"rgb565"`
- `opts.width`, `opts.height`: full source buffer size
- `opts.mode`: `"raw"`, `"fit"`, `"cover"`, `"stretch"`, or `"crop"`; default is `"raw"`
- `opts.dst_width`, `opts.dst_height`: destination size for stretch/cover/crop modes
- `opts.max_width`, `opts.max_height`: fit box; accepted as aliases for fit destination size
- `opts.source`: `{ x, y, width, height }` source rectangle. In raw mode this draws the source rectangle without scaling.
- Returns `output_w, output_h`

Examples:

```lua
display.draw_pixels(0, 0, rgb565_bytes, {
    format = "rgb565",
    width = 320,
    height = 240,
})

display.draw_pixels(0, 0, rgb565_bytes, {
    format = "rgb565",
    width = 320,
    height = 240,
    mode = "fit",
    max_width = display.width,
    max_height = display.height,
})

display.draw_pixels(0, 0, rgb565_bytes, {
    format = "rgb565",
    width = 320,
    height = 240,
    mode = "crop",
    source = { x = 40, y = 20, width = 160, height = 120 },
    dst_width = 320,
    dst_height = 240,
})
```

### `display.draw_image(x, y, frame, opts)`

Draws an `image.frame` directly. The display module requests RGB565 from the
image module and borrows the buffer only during the C call, avoiding the large
Lua string copy caused by `frame:data()`.

`opts` is optional:

- `mode`: `"raw"`, `"fit"`, `"cover"`, `"stretch"`, or `"crop"`; default is `"raw"`
- `width`, `height`: destination size for fit/cover/stretch/crop modes
- `source`: `{ x, y, width, height }` source rectangle for crop/cover modes

Examples:

```lua
display.draw_image(0, 0, frame, {
    mode = "fit",
    width = display.width,
    height = display.height,
})

display.draw_image(0, 0, frame, {
    mode = "crop",
    source = { x = 20, y = 20, width = 160, height = 120 },
    width = 320,
    height = 240,
})
```

- Returns `output_w, output_h`
- The call is synchronous and does not retain the image buffer after returning

## Error behavior and constraints

- Most APIs raise Lua errors directly when arguments are invalid or the HAL returns an error
- Integer-only APIs reject non-integer Lua values
- File path validation is handled by the `image` module when loading or saving image files
- `draw_pixels(...)` rejects buffers that are too short
- `draw_text(...)` and `draw_text_aligned(...)` reject non-ASCII text

## Recommended usage pattern

For normal screen rendering:
1. Use `board_manager.get_display_lcd_params("display_lcd")`
2. Call `display.init(...)`
3. Call `display.begin_frame(...)`
4. Draw text, shapes, or images
5. Call `display.present()` or `display.present_full()`
6. Call `display.end_frame()`
7. Call `display.deinit()` before exit

## Example

```lua
local bm = require("board_manager")
local display = require("display")

local panel_handle, io_handle, width, height, panel_if =
    bm.get_display_lcd_params("display_lcd")

display.init(panel_handle, io_handle, width, height, panel_if)

display.begin_frame({ clear = true, color = "#0c1220" })

display.draw_rect(12, 12, display.width - 24, display.height - 24, { r = 80, g = 120, b = 160 })
display.fill_rect(20, 40, 80, 36, "#48d0eb")
display.draw_text(24, 90, "Lua Display Demo", {
    color = "#f5f4ee",
    font_size = 24,
})
display.draw_text_aligned(0, display.height - 24, display.width, 20, "frame api", {
    color = { r = 210, g = 220, b = 228 },
    font_size = 16,
    align = "center",
    valign = "middle",
})

display.present()
display.end_frame()
display.deinit()
```
