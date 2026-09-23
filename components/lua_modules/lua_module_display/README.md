# `display` Lua API

`display` is an immediate-mode 2D drawing API for the device's built-in screen. Use it to render games, dashboards, camera overlays, and custom interfaces. It does not provide widgets or an external-screen API. One Lua job owns the screen at a time, and the returned screen must only be used by the Lua job that opened it.

Signatures below use `->` to show return values. Functions without `->` return no values.

## Quick start

```lua
local display = require("display")
local screen <close> = display.open()
local info = screen:info()
local panel_color = display.color(20, 90, 130)

screen:begin({ clear = "#101820" })
screen:fill_round_rect(12, 12, info.width - 24, 60, 8, panel_color)
screen:text(24, 30, "hello", { font_size = 24, color = 0xFFFFFFFF })
screen:present()
```

The drawing sequence is always `begin()` → draw → `present()`. The screen is released by `screen:close()`, Lua's `<close>` scope, or job exit. During a running job, dropping the Lua variable alone does not release it. Keep the screen open while its scene should remain visible.

## Open and screen information

`display.open([options]) -> screen` opens the built-in screen. Calling it again before the current screen closes raises `display already open`.

| Option | Default | Meaning |
| --- | --- | --- |
| `framebuffer_count` | `1` | `1` or `2`; use `2` only when the extra memory is justified by the scene |

The drawing format comes from the built-in screen's color depth. The display service handles any panel-interface byte-order conversion; Lua code does not configure it.

`screen:info()` returns:

| Field | Meaning |
| --- | --- |
| `width`, `height` | Screen dimensions in pixels |
| `pixel_format` | Active drawing format: `"rgb565"` or `"rgb888"` |
| `bytes_per_pixel` | `2` for RGB565 or `3` for RGB888 |
| `framebuffer_count` | Active framebuffer count |
| `framebuffer_bytes` | Total bytes reserved by all framebuffers |
| `touch_available` | Whether the built-in screen has touch input |

Cache this table if it is needed every frame.

`screen:stats()` returns the latest frame statistics:

| Field | Meaning |
| --- | --- |
| `draw_us` | Time from the start of `begin()` until `present()` begins, including framebuffer synchronization, clearing, Lua scene work, and drawing |
| `present_us` | Time spent submitting pixels and waiting for presentation |
| `sync_us` | Portion of `draw_us` spent synchronizing two framebuffers |
| `dirty_pixels` | Number of submitted pixels |
| `dirty_rects` | Number of submitted rectangles |
| `submitted_bytes` | Submitted pixel bytes before panel byte-order conversion |
| `framebuffer_bytes` | Total memory reserved by all framebuffers |

A frame with no update reports zero presentation time, rectangles, pixels, and submitted bytes.

`screen:close()` is safe to call more than once. Other screen methods raise an error after close.

## Frames and drawing state

```lua
screen:begin({ clear = 0xFF0C1520 })
screen:save()
screen:translate(20, 40)
screen:clip(0, 0, 120, 80)
screen:fill_rect(0, 0, 120, 80, 0xCC183047)
screen:restore()
local updated = screen:present()
```

- `screen:begin([{ clear = color }])` starts a frame. Omit `clear` to preserve the previous image; the first drawn frame starts from black. Starting another frame before presenting the current one is an error.
- `screen:present([{ full = false }]) -> boolean` finishes the frame and returns `true` if pixels were submitted. A frame with no changes normally returns `false`; `full = true` requests a full-screen refresh. If presentation fails, the frame remains active so it can be retried or the screen can be closed.
- `screen:save()` / `screen:restore()` save and restore translation and clipping. The stack holds at most eight saved states; restoring without a matching save is an error.
- `screen:translate(dx, dy)` adds an integer offset to subsequent drawing coordinates. `screen:clip(x, y, w, h)` intersects the current clip with a rectangle in the translated coordinate space. A negative clip width or height is an error; zero creates an empty clip. Drawing state resets at each `begin()`.

Drawing, translation, clipping, save, and restore require an active frame. `info()`, `stats()`, `measure_text()`, and `touch()` do not.

## Colors and shapes

Every shape method accepts a `color` in one of these forms:

- A packed `0xAARRGGBB` integer. On devices with 32-bit Lua integers, values with the high bit set may appear negative but retain the same color bits.
- `"#rrggbb"` or `"#rrggbbaa"`.
- `{ r = 255, g = 80, b = 40, a = 192 }` or `{ 255, 80, 40, 192 }`; omitted alpha defaults to 255.

`display.color(r, g, b[, a]) -> integer` packs components in `0..255` into a reusable color; `a` defaults to 255. Alpha 0 draws nothing, alpha 255 is opaque, and intermediate alpha values blend over the current image. A packed literal must include its alpha byte: use `0xFFFF0000` for opaque red, not `0xFF0000`. Six-digit color strings such as `"#ff0000"` are opaque.

Positions, dimensions, and radii are integers. The origin is at the top left, x grows rightward, and y grows downward. Drawing is clipped to the screen and current clip.

```lua
screen:fill_rect(x, y, w, h, color)
screen:stroke_rect(x, y, w, h, color)
screen:line(x0, y0, x1, y1, color)
screen:fill_circle(cx, cy, radius, color)
screen:stroke_circle(cx, cy, radius, color)
screen:arc(cx, cy, radius, start_deg, end_deg, color)
screen:fill_round_rect(x, y, w, h, radius, color)
screen:stroke_round_rect(x, y, w, h, radius, color)
screen:fill_triangle(x1, y1, x2, y2, x3, y3, color)
```

Arc angles are finite numbers and increase clockwise in screen coordinates.
Outlines are one pixel wide. Non-positive rectangle dimensions and negative radii draw nothing. For arcs, a sweep of at least 360 degrees draws a full circle, equal start and end angles draw nothing, and a negative difference wraps clockwise through 360 degrees.

## Text and fonts

```lua
local w, h = screen:measure_text("status", { font_size = 24 })
screen:text(8, 8, "status", { font_size = 24, color = "#ffffff" })

local font <close> = display.load_font(font_path)
screen:text(8, 40, "Temperature", { font = font, color = "#ffffff" })
```

`screen:text(x, y, text[, options])` draws a valid UTF-8 string inside an active frame. `screen:measure_text(text[, options]) -> width, height` uses the same sizing rules and can be called outside a frame. Both accept `color` (default white), `font_size` (default 24), and `font`.

Without `font`, the built-in font supports printable ASCII at integer sizes 8–64. Text is not automatically wrapped or aligned: `\n` starts a new line, `\r` returns to the start of the current line, and `\t` advances by four glyph widths. An empty string measures `0, 0`.

For other Unicode characters, load a readable DFN1 bitmap font with `display.load_font(path) -> font`. DFN1 preserves each glyph's advance, bounding box, baseline offset, and Unicode encoding. A custom font is rendered at its stored size, so `font_size` is ignored when `font` is present. A missing glyph uses the font's configured fallback glyph if available; otherwise drawing and measurement raise an error. Invalid UTF-8 also raises an error. `font:close()` is idempotent; a closed font cannot be used. Close the font only after its final synchronous `text()` or `measure_text()` call.

The built-in `bdf_font_converter` skill converts horizontal Unicode BDF 2.1/2.2 files from writable storage into DFN1 on the device. The format limits the line height, glyph width, glyph height, and horizontal advance to 64 pixels, with at most 4096 glyphs and a 1 MiB file size.

## Images and raw pixels

```lua
local image = require("image")
local frame <close> = image.load_file(image_path)
local red_tile = string.rep("\0\248", 64 * 64)
screen:begin()
screen:image(0, 0, frame, { mode = "contain", width = 120, height = 90, opacity = 220 })
screen:blit(130, 0, red_tile, { width = 64, height = 64, format = "rgb565" })
screen:present()
```

`screen:image(x, y, image_frame[, options])` accepts an `image.frame` from the `image` module. The frame must remain valid during the call. The image is drawn synchronously; it is not retained by `display`.

| `mode` | Result |
| --- | --- |
| `"raw"` (default) | Draw the selected source region at its natural size; ignore target `width` and `height` |
| `"contain"` | Fit inside the target box without distortion, centered |
| `"cover"` | Fill the target box without distortion, cropping from the center |
| `"stretch"` | Fill the target box, allowing distortion |
| `"crop"` | Draw from the source region's top left without scaling, limited by the target size |

Image options also include `source = { x = 0, y = 0, width = 32, height = 24 }` to select a source region, and `opacity` in `0..255` (default 255). Source coordinates must be inside the image and the selected region must fit. Omitted source dimensions extend to the source image's edge. For non-`raw` modes, omitted or zero target dimensions default to the selected source dimensions. Opacity 0 draws nothing.

The target box is not cleared automatically. For example, `contain` leaves any uncovered part of the box unchanged.

`screen:blit(x, y, bytes, options)` draws a Lua string of raw pixels. `options` must specify positive integer `width` and `height`, plus `format`: `"rgb565"` (little-endian pixels), `"rgb888"` (RGB byte order), or `"bgr888"` (BGR byte order). The string length must equal `width * height * bytes_per_pixel` exactly. `blit()` does not scale or apply opacity; use `image()` for those operations. Raw pointers are not accepted.

Both `image()` and `blit()` obey the current translation and clip. Their coordinates identify the destination's top-left corner.

## Touch and errors

`screen:touch() -> { points = { { id = number, x = number, y = number }, ... } }` returns the latest touch snapshot in screen coordinates. `points` contains every active touch point in provider order and is empty when nothing is touching the screen. Track a gesture with `id`; do not assume the array position remains stable. On a board without built-in touch, the call raises a not-supported error; check `screen:info().touch_available` first.

Invalid arguments, invalid UTF-8, invalid frame state, a closed screen or font, and unavailable hardware raise Lua errors. Use `pcall` when a scene should recover from an expected failure. Tests and runnable examples are in [`test/`](test/).

For animation, load images and fonts once, reuse prepared colors and pixel strings, and submit each frame with one `begin()` / `present()` pair.

## Performance benchmark

[`test/display_benchmark.lua`](test/display_benchmark.lua) measures the public display API on the real screen. It covers open-to-ready latency, framebuffer memory, partial and full presentation, a mixed scene, rectangle fill, lines, circles, text, raw-pixel blitting, close cleanup, and both one- and two-framebuffer modes. The same script detects the legacy or current API so results from before and after the refactor remain comparable.

Run it as an asynchronous managed Lua job because it owns the screen for the duration of the test. Firmware-baked tests are available through the `builtin_lua_modules` skill; pass the absolute path that skill provides:

```text
lua --run-async --path <absolute-path-to-display_benchmark.lua> --args-json "{\"label\":\"candidate\"}" --timeout-ms 180000
```

The optional JSON arguments are:

| Argument | Default | Range or meaning |
| --- | --- | --- |
| `label` | `"unspecified"` | Identifier copied to every output record |
| `repeats` | `3` | `1..7`; the median repeat is reported |
| `warmup_iterations` | `3` | `1..20` |
| `frame_iterations` | `24` | `4..200` |
| `raster_iterations` | `800` | `100..10000` |
| `framebuffer_count` | `0` | `0` tests both modes; otherwise `1` or `2` |

Machine-readable records start with `DISPLAY_BENCH|`. `result` records report `median_us`, `min_us`, `max_us`, `us_per_op`, `ops_per_s`, and frame-workload `pixels_per_s`. `memory` records report `open_us`, `ready_us`, framebuffer bytes, and SRAM/PSRAM use. `memory_after_close` shows the remaining heap delta, and a successful run ends with `status=PASS`.

For a valid comparison, use the same board, display interface, pixel format, byte-order defaults, CPU frequency, ESP-IDF revision, benchmark arguments, and background workload. Let startup networking and time synchronization settle before each run. Lower latency and memory values are better; higher throughput values are better.
