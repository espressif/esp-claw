# display_shape.lua

Pure-Lua geometry for the visible area of rectangular, rounded-rectangle and circular panels. It only computes coordinates, so it works with both `lvgl` and `display` scripts. The shape comes from `board_manager.get_display_shape()` unless the caller overrides it.

## Require

```lua
local display_shape = require("display_shape")
```

The script's own directory is searched first, so do not name a script `display_shape.lua`.

## Functions

### `display_shape.new(width, height[, opts])`

Creates a frame for a screen of `width` x `height` pixels. Pass the size reported by the screen, for example `screen:get_size()` in `lvgl` or `screen:info()` in `display`.

- `opts.shape`: `rect`, `rounded_rect` or `circle`. When omitted, the board shape is used; without board information the frame is a rectangle.
- `opts.corner_radius`: corner radius in pixels, used only with `opts.shape = "rounded_rect"`. It is clamped to half of the shorter side.

A circle uses the shorter side as its diameter and is centered on the screen. Invalid sizes, unknown shapes or a missing `corner_radius` raise errors that start with `display_shape:`.

## Fields

- `frame.shape`: `rect`, `rounded_rect` or `circle`.
- `frame.width`, `frame.height`: the screen size passed to `new()`.
- `frame.radius`: effective corner radius; half of the diameter for a circle and `0` for a rectangle.

## Methods

### `frame:span(y, h)`

Returns `x, w`: the horizontal range that is fully visible on every line of the band `[y, y + h)`. A rectangle always returns `0, width`. A band outside the visible area returns a width of `0`.

### `frame:safe_rect()`

Returns `x, y, w, h`: the largest centered axis-aligned rectangle whose corners touch each corner arc at 45 degrees. A circle gives its inscribed square; a rectangle gives the whole screen.

### `frame:top_for(w, h)`

Returns the smallest `y` whose band `[y, y + h)` has a span of at least `w`, or `nil` when no such band exists. Use it to place a header row as high as the outline allows.

### `frame:bottom_for(w, h)`

Returns the largest `y` whose band `[y, y + h)` has a span of at least `w`, or `nil`. Use it to place a footer row as low as the outline allows.

## Example

```lua
local lvgl = require("lvgl")
local display_shape = require("display_shape")

lvgl.init()
local screen = lvgl.create_screen()
local width, height = screen:get_size()
local frame = display_shape.new(width, height)

local bar_h = 40
local bar_y = frame:bottom_for(160, bar_h) or (height - bar_h)
local x, w = frame:span(bar_y, bar_h)
lvgl.button(screen, { text = "OK", x = x, y = bar_y, w = w, h = bar_h })
```
