# Lua LVGL Usage Guide

This document is an API guide for building LVGL UIs from Lua with the `lvgl` module.

## Core Rules

- Import the module with `local lvgl = require("lvgl")`.
- Call `lvgl.init([opts])` before creating screens or widgets.
- All widget operations are userdata methods. Use `btn:set_text("OK")`, not `lvgl.set_text(btn, "OK")`.
- Only one Lua script can own the LVGL runtime at a time. Do not use `display.init(...)` and `lvgl.init(...)` together.
- Call `lvgl.deinit()` before the script exits. The module also cleans up automatically if the owner script exits unexpectedly.
- Object handles become invalid after `obj:delete()`, after a parent object is deleted, or after `lvgl.deinit()`.
- After registering events, call `lvgl.run()` or repeatedly call `lvgl.process_events(...)`; otherwise Lua callbacks will not run.

## Minimal Example

```lua
local lvgl = require("lvgl")

lvgl.init({
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
})

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101820" })

local label = lvgl.label(scr, {
    text = "LVGL from Lua",
    align = "top_mid",
    y = 20,
    text_color = "#ffffff",
})

local btn = lvgl.button(scr, {
    text = "OK",
    align = "center",
    w = 120,
    h = 44,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

btn:on("clicked", function()
    label:set_text("clicked")
end)

scr:load()
lvgl.run()
lvgl.deinit()
```

## Init And Deinit

```text
lvgl.init([opts]) -> true
```

Common `opts`:
- `buffer_lines`: draw buffer height in lines, default `40`
- `tick_ms`: LVGL tick period, default `5`
- `task_period_ms`: LVGL handler task period, default `10`
- `font_path`: default runtime font path, default `fonts/NotoSansSC-Regular-sub.ttf`; DATA is tried first, then SYSTEM
- `font_size`: default runtime font size, default `24`
- `font_cache_size`: default runtime font glyph cache size, default `LV_TINY_TTF_CACHE_GLYPH_CNT`

Legacy scripts may still call `lvgl.init(panel_handle, io_handle, width, height, panel_if, opts)`. The positional display parameters are ignored; only `opts` is used.

Legacy constants are still exported for older scripts:

- `lvgl.PANEL_IF_IO`
- `lvgl.PANEL_IF_RGB`
- `lvgl.PANEL_IF_MIPI_DSI`

They are not needed by `lvgl.init([opts])`.

Shutdown:

```text
lvgl.deinit() -> true
```

## Touch Input

Register a touch panel as an LVGL input device:

```lua
local board_manager = require("board_manager")

local touch_handle, err = board_manager.get_lcd_touch_handle("lcd_touch")
if touch_handle then
    lvgl.indev_register("touch", touch_handle) -- -> true
end
```

Unregister it before shutdown when needed:

```lua
lvgl.indev_unregister("touch") -- -> removed
```

`indev_register("touch", ...)` does not take ownership of the touch handle. `indev_unregister("touch")` returns `true` only when a touch input device was actually removed.

## Event Loop

Register callbacks with `obj:on(event, callback)`:

```lua
local handle = btn:on("clicked", function()
    print("clicked")
end)
```

Remove callbacks:

```lua
btn:off(handle)      -- -> removed_count
btn:off("clicked")  -- -> removed_count
btn:off()           -- -> removed_count
```

Supported event names:

`clicked`, `pressed`, `released`, `long_pressed`, `value_changed`,
`focused`, `defocused`, `ready`, `cancel`

Drive the event loop:

```text
lvgl.run([{ period_ms = 200 }]) -> processed_count
```

Or:

```lua
while true do
    lvgl.process_events(50) -- -> processed_count
    -- run other periodic Lua-side work here
end
```

`lvgl.process_events([timeout_ms = 0])` treats negative timeouts as `0`. Event callbacks receive no arguments; callback errors are logged and event dispatch continues.

## Widget Constructors

All constructors follow the same basic shape:

```lua
local obj = lvgl.widget(parent, opts) -- -> object userdata
```

Basic widgets:

- `lvgl.object(parent, opts)`
- `lvgl.container(parent, opts)`
- `lvgl.label(parent, opts)`
- `lvgl.button(parent, { text = "OK" })`
- `lvgl.bar(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.slider(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.arc(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.scale(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.checkbox(parent, { text = "Enable", checked = true })`
- `lvgl.switch(parent, { checked = true })`
- `lvgl.dropdown(parent, { options = {"A", "B"}, selected = 1 })`
- `lvgl.roller(parent, { options = {"A", "B"}, selected = 1 })`
- `lvgl.keyboard(parent, { mode = "text_lower", textarea = textarea })`
- `lvgl.textarea(parent, { text = "..." })`
- `lvgl.list(parent, opts)`
- `lvgl.table(parent, { rows = 2, cols = 2 })`
- `lvgl.image(parent, { src = "<your/path/image.bin>" })`
- `lvgl.line(parent, { points = {{x=0,y=0}, {x=20,y=20}} })`
- `lvgl.spinner(parent, { anim_ms = 1000, arc_sweep = 60 })`
- `lvgl.buttonmatrix(parent, { map = {"1", "2", "\n", "3"}, one_checked = true })`
- `lvgl.calendar(parent, { today = {2026, 5, 15}, shown = {2026, 5}, highlighted = {{2026, 5, 15}} })`
- `lvgl.canvas(parent, { w = 80, h = 40, color_format = "rgb565" })`
- `lvgl.chart(parent, { type = "line", point_count = 10, min = 0, max = 100, update_mode = "shift" })`
- `lvgl.imagebutton(parent, { src = "<your/path/image.bin>" })`
- `lvgl.led(parent, { color = "#00ff00", brightness = 180, on = true })`
- `lvgl.menu(parent, opts)`
- `lvgl.msgbox(parent_or_nil, { title = "...", text = "...", buttons = {"OK"}, close_button = true })`
- `lvgl.spangroup(parent, { mode = "break", overflow = "ellipsis", spans = {"A", "B"} })`
- `lvgl.spinbox(parent, { min = 0, max = 100, value = 10, step = 1 })`
- `lvgl.tabview(parent, { tab_bar_position = "top", tab_bar_size = 36 })`
- `lvgl.tileview(parent, opts)`
- `lvgl.window(parent, opts)`
- `lvgl.eaf(parent, { src = "<your/path/anim.eaf>", loop_count = -1, loop_enabled = true })`

Lua index convention:
- dropdown/roller selected indexes are 1-based
- table rows and columns are 1-based
- buttonmatrix selected indexes are 1-based
- tabview active indexes are 1-based
- tileview `col` and `row` are 1-based

Widget-specific `opts`:

- `image`: `src`
- `line`: `points`, `y_invert`
- `arc`: `start_angle`, `end_angle`, `bg_start_angle`, `bg_end_angle`, `rotation`, `mode`
- `spinner`: `anim_ms`, `arc_sweep`
- `scale`: `mode`, `total_ticks`, `major_tick_every`, `label_show`, `angle_range`, `rotation`
- `checkbox`, `switch`: `checked`
- `dropdown`: `options`, `selected`, `dir`, `symbol`
- `roller`: `options`, `selected`, `mode`, `visible_rows`
- `keyboard`: `mode`, `popovers`, `textarea`
- `textarea`: `placeholder`, `one_line`, `password`, `max_length`, `accepted_chars`
- `table`: `rows`, `cols`, `cells`, `column_widths`
- `buttonmatrix`: `map`, `one_checked`
- `calendar`: `today`, `shown`, `highlighted`, `day_names`
- `canvas`: `w`, `h`, `color_format`
- `chart`: `type`, `point_count`, `min`, `max`, `update_mode`
- `imagebutton`: `src`
- `led`: `color`, `brightness`, `on`
- `msgbox`: `title`, `text`, `buttons`, `close_button`
- `spangroup`: `mode`, `overflow`, `indent`, `max_lines`, `spans`
- `spinbox`: `min`, `max`, `value`, `step`, `digit_count`, `dec_point_pos`, `rollover`
- `tabview`: `tab_bar_position`, `tab_bar_size`
- `eaf`: `src`, `src_data`, `loop_count`, `loop_enabled`, `frame_delay`

## Common Options

Most widgets support:

- Position and size: `x`, `y`, `w`, `h`, `align`
- Text: `text`
- Numeric values: `min`, `max`, `value`
- Style: `bg_color`, `text_color`, `border_color`, `bg_opa`, `opa`,
  `radius`, `border_width`, `pad`, `pad_row`, `pad_column`,
  `line_color`, `line_width`, `arc_width`, `font`

Colors can be strings or numbers:

```lua
bg_color = "#2f80ed"
text_color = 0xffffff
```

## Runtime TTF Fonts

By default, `lvgl.init()` tries to load `fonts/NotoSansSC-Regular-sub.ttf` as the runtime font and applies it to the root screen and every screen created with `lvgl.create_screen()`. If that font is unavailable, LVGL uses its built-in font. Set `font_size` per app when a different default text size is needed:

```lua
lvgl.init({
    font_size = 22,
})
```

When LVGL `tiny_ttf` is enabled, fonts can be loaded from the DATA root at
runtime for per-object overrides:

```lua
local storage = require("storage")
local lvgl = require("lvgl")

local font_path = storage.join_path(storage.get_root_dir(), "fonts/NotoSansSC-Regular.ttf")
local font = lvgl.font_load(font_path, { size = 24, cache_size = 128 })
label:set_style({ font = font })
```

- `lvgl.font_load(path[, { size = 16, cache_size = LV_TINY_TTF_CACHE_GLYPH_CNT }])` -> font handle
- `font:set_size(px)` -> true
- `font:is_valid()` -> boolean
- `font:delete()` -> true

`lvgl.init({ font_path = ... })` resolves the path from DATA first, then SYSTEM. `lvgl.font_load(path[, opts])` only accepts a DATA-relative path, a `D:/...` LVGL filesystem path, or an absolute path under the DATA root. Runtime font files must remain available while any LVGL object uses them.

## EAF Animation

EAF playback is exposed as an LVGL object:

```lua
local anim = lvgl.eaf(scr, {
    src = "<your/path/idle.eaf>",
    align = "center",
    loop_count = -1,
    loop_enabled = true,
    frame_delay = 100,
})

if anim:is_loaded() then
    anim:pause()
    anim:resume()
end
```

Methods:
- `anim:set_src(path)`
- `anim:set_src_data(binary_string)`
- `anim:restart()`, `anim:pause()`, `anim:resume()`
- `anim:is_loaded()`
- `anim:get_total_frames()`, `anim:get_current_frame()`
- `anim:set_loop_count(n)`, `anim:get_loop_count()`
- `anim:set_loop_enabled(boolean)`, `anim:get_loop_enabled()`
- `anim:set_frame_delay(ms)`, `anim:get_frame_delay()`

Use either `src` or `src_data` when creating an EAF object.

## Common Methods

All LVGL object userdata supports:

- Methods that change object state return `true` on success unless another return value is shown.
- `obj:set_pos(x, y)`
- `obj:get_pos()` -> `x, y`
- `obj:set_size(w, h)`
- `obj:get_size()` -> `w, h`
- `obj:align(name[, x, y])`
- `obj:is_valid()` -> boolean
- `obj:set_style(opts)`
- `obj:set_flex(opts)`
- `obj:set_grid(opts)`
- `obj:set_grid_cell(opts)`
- `obj:set_scroll(opts)`
- `obj:on(event, callback)` -> handle
- `obj:off([handle_or_event])` -> removed_count
- `obj:delete()`
- `obj:clean()`

Screen helpers:
- `lvgl.screen()` -> active screen object
- `lvgl.create_screen()` -> screen object

Common `align` names:

`top_left`, `top_mid`, `top`, `top_right`, `bottom_left`, `bottom_mid`,
`bottom`, `bottom_right`, `left_mid`, `left`, `right_mid`, `right`,
`center`, `centre`, `default`

## Layout And Scrolling

Flex:

```lua
obj:set_flex({
    flow = "column",
    main = "start",
    cross = "center",
    track = "start",
})
```

`flow`: `row`, `column`, `row_wrap`, `row_reverse`, `row_wrap_reverse`,
`column_wrap`, `column_reverse`, `column_wrap_reverse`

`main/cross/track`: `start`, `center`, `end`, `space_between`,
`space_around`, `space_evenly`

Grid:

```lua
obj:set_grid({
    cols = {"fr", "fr"},
    rows = {"content", 40},
    col_align = "stretch",
    row_align = "start",
})
```

Grid cell:

```lua
obj:set_grid_cell({
    col = 1,
    row = 1,
    col_span = 1,
    row_span = 1,
    col_align = "stretch",
    row_align = "stretch",
})
```

Scroll:

```lua
obj:set_scroll({
    dir = "ver",
    scrollbar = "auto",
    snap_x = "none",
    snap_y = "none",
})
```

`dir`: `none`, `left`, `right`, `top`, `bottom`, `hor`, `ver`, `all`

`scrollbar`: `auto`, `off`, `on`, `active`

`snap_x/snap_y`: `none`, `start`, `end`, `center`

## Type-Specific Methods

Basic methods:

- `label/button/checkbox/dropdown/textarea/list_text/list_button:set_text(text)`
- `bar/slider/roller:set_value(v[, anim])`
- `arc/scale/dropdown/checkbox/switch/spinbox:set_value(v)`
- `bar/slider/arc/scale/dropdown/roller/checkbox/switch/spinbox:get_value()`
- `bar/slider/arc/scale/spinbox:set_range(min, max)`
- `screen:load()`
- `list:add_text(text)` -> `list_text`
- `list:add_button(text[, symbol])` -> `list_button`
- `table:set_cell(row, col, text)`
- `table:get_cell(row, col)` -> string
- `buttonmatrix:set_map(map)`
- `buttonmatrix:set_selected(index)`
- `buttonmatrix:get_selected()` -> index or nil
- `buttonmatrix:get_button_text(index)` -> string
- `buttonmatrix:set_one_checked(bool)`
- `calendar:set_today(y, m, d)`
- `calendar:set_shown(y, m)`
- `calendar:set_highlighted({{y,m,d}, ...})`
- `calendar:get_pressed_date()` -> `{ year = y, month = m, day = d }` or nil
- `canvas:fill_bg(color[, opa])`
- `canvas:set_px(x, y, color[, opa])`
- `canvas:set_rgb565_data(data[, byte_order])`
- `canvas:get_px(x, y)` -> `{r, g, b, a}`
- `chart:add_series(color[, axis])` -> series handle
- `chart:set_type(type)`
- `chart:set_point_count(n)`
- `chart:set_range(min, max[, axis])`
- `chart:set_next_value(series, value)`
- `chart:set_series_values(series, values)`
- `chart:refresh()`
- `imagebutton:set_src(state, mid[, left, right])`
- `imagebutton:set_state(state)`
- `led:set_color(color)`
- `led:set_brightness(v)`
- `led:get_brightness()` -> integer
- `led:on()`, `led:off()`, `led:toggle()`
- `menu:page([title])` -> page
- `menu:cont(parent)` -> cont
- `menu:section(page)` -> section
- `menu:separator(page)` -> separator
- `menu:set_page([page])`
- `menu:set_sidebar_page([page])`
- `menu:set_mode_header(mode)`
- `menu:set_root_back_button(bool)`
- `menu:clear_history()`
- `msgbox:add_title(text)` -> msgbox_child
- `msgbox:add_text(text)` -> msgbox_child
- `msgbox:add_footer_button(text)` -> msgbox_child
- `msgbox:add_close_button()` -> msgbox_child
- `msgbox:close()`
- `msgbox:close_async()`
- `spangroup:add_span(text[, style])` -> span handle
- `spangroup:get_span_count()` -> integer
- `spangroup:refresh()`
- `span:set_text(text)`
- `span:get_text()` -> string
- `span:set_style(opts)`
- `span:delete()`
- `spinbox:set_step(v)`
- `spinbox:get_step()` -> integer
- `spinbox:increment()`
- `spinbox:decrement()`
- `spinbox:step_next()`
- `spinbox:step_prev()`
- `tabview:add_tab(name)` -> tab page
- `tabview:set_active(index[, anim])`
- `tabview:get_active()` -> index
- `tabview:get_tab_count()` -> integer
- `tabview:set_tab_text(index, text)`
- `tileview:add_tile(col, row[, dir])` -> tile
- `tileview:set_tile(tile[, anim])`
- `tileview:set_tile_by_index(col, row[, anim])`
- `tileview:get_active_tile()` -> tile or nil
- `window:add_title(text)` -> label
- `window:add_button([icon[, width]])` -> button
- `window:get_header()` -> object
- `window:get_content()` -> object

`span:set_style(opts)` applies style options through the owning `spangroup`.

- `canvas.color_format`: `rgb565`, `rgb888`, `xrgb8888`, `argb8888`, `native`
- `chart.type`: `none`, `line`, `curve`, `bar`, `stacked`, `scatter`
- `chart.update_mode`: `shift`, `circular`
- `chart` axis: `primary_y`, `y`, `secondary_y`, `primary_x`, `x`, `secondary_x`
- `imagebutton` state: `released`, `pressed`, `disabled`,
  `checked_released`, `checked_pressed`, `checked_disabled`
- `spangroup.mode`: `fixed`, `expand`, `break`
- `spangroup.overflow`: `clip`, `ellipsis`
- `menu` header mode: `top_fixed`, `top_unfixed`, `bottom_fixed`
- Direction values: `none`, `left`, `right`, `top`, `bottom`, `hor`, `ver`, `all`

## Demos

- `lvgl.demos()` -> `{name, ...}`
- `lvgl.demo(name)` -> `true`

`lvgl.demo(name)` requires an initialized LVGL runtime. Available demo names depend on firmware LVGL demo build flags.

## Limitations

- Encoder/keypad indevs are not exposed yet.
- Some widgets and demos depend on LVGL `LV_USE_*` build flags; disabled entries raise a Lua error such as `not enabled in firmware`.
- `lvgl.font_load(...)` requires LVGL `tiny_ttf` support.
- Image decoders and general filesystem setup are not wrapped.
- `lvgl.image(...)` and `lvgl.imagebutton(...)` only pass string `src` values
  to LVGL. Whether those strings load depends on firmware FS/decoder setup.
- Canvas support covers buffer allocation, background fill, and pixel read/write only. Advanced draw layers are not wrapped.
- Chart cursors and other advanced chart APIs are not wrapped.
- Span handles and chart series handles are not LVGL objects; they do not support object base methods such as `set_pos`, `set_style`, or `delete`.
- Non-ASCII text rendering depends on either firmware-enabled fonts or a
  runtime TTF font applied with `font`.
