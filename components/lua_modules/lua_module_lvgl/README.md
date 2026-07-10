# Lua LVGL Usage Guide

This document is written for LLMs and Lua script generation. It explains how
to use the `lvgl` module exposed by `components/lua_modules/lua_module_lvgl`.

## Core Rules

- Import the module with `local lvgl = require("lvgl")`.
- Get display parameters with `board_manager.get_display_lcd_params("display_lcd")`, then call `lvgl.init(...)`.
- All widget operations are userdata methods. Use `btn:set_text("OK")`, not `lvgl.set_text(btn, "OK")`.
- Only one Lua script can own the LVGL runtime at a time. Do not use `display.init(...)` and `lvgl.init(...)` together.
- Call `lvgl.deinit()` before the script exits. The module also cleans up automatically if the owner script exits unexpectedly.
- Object handles become invalid after `obj:delete()`, after a parent object is deleted, or after `lvgl.deinit()`.
- After registering events, call `lvgl.run()` or repeatedly call `lvgl.process_events(...)`; otherwise Lua callbacks will not run.

## Minimal Example

```lua
local board_manager = require("board_manager")
local lvgl = require("lvgl")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
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

```lua
lvgl.init(panel_handle, io_handle, width, height, panel_if, opts)
```

Common `opts`:
- `buffer_lines`: draw buffer height in lines, default `40`
- `tick_ms`: LVGL tick period, default `5`
- `task_period_ms`: LVGL handler task period, default `10`

Shutdown:

```lua
lvgl.deinit()
```

## Touch Input

Register a touch panel as an LVGL input device:

```lua
local touch_handle, err = board_manager.get_lcd_touch_handle("lcd_touch")
if touch_handle then
    lvgl.indev_register("touch", touch_handle)
end
```

Unregister it before shutdown when needed:

```lua
lvgl.indev_unregister("touch")
```

`indev_register("touch", ...)` borrows the `esp_lcd_touch_handle_t`; it does
not free the underlying touch handle.

## Event Loop

Register callbacks with `obj:on(event, callback)`:

```lua
local handle = btn:on("clicked", function()
    print("clicked")
end)
```

Remove callbacks:

```lua
btn:off(handle)      -- remove one callback handle
btn:off("clicked")  -- remove all callbacks for this event
btn:off()           -- remove all callbacks from this object
```

Supported event names:

`clicked`, `pressed`, `released`, `long_pressed`, `value_changed`,
`focused`, `defocused`, `ready`, `cancel`

Drive the event loop:

```lua
lvgl.run()
```

Or:

```lua
while true do
    lvgl.process_events(50)
    -- run other periodic Lua-side work here
end
```

## Widget Constructors

All constructors follow the same basic shape:

```lua
local obj = lvgl.widget(parent, opts)
```

Basic widgets:

- `lvgl.object(parent, opts)`
- `lvgl.container(parent, opts)`
- `lvgl.label(parent, opts)`
- `lvgl.button(parent, { text = "OK" })`
- `lvgl.bar(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.slider(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.arc(parent, opts)`
- `lvgl.scale(parent, opts)`
- `lvgl.checkbox(parent, { text = "Enable", checked = true })`
- `lvgl.switch(parent, { checked = true })`
- `lvgl.dropdown(parent, { options = {"A", "B"}, selected = 1 })`
- `lvgl.roller(parent, { options = {"A", "B"}, selected = 1 })`
- `lvgl.keyboard(parent, { mode = "text_lower", textarea = textarea })`
- `lvgl.textarea(parent, { text = "..." })`
- `lvgl.list(parent, opts)`
- `lvgl.table(parent, opts)`
- `lvgl.image(parent, { src = "S:/path.bin" })`
- `lvgl.line(parent, { points = {{x=0,y=0}, {x=20,y=20}} })`
- `lvgl.spinner(parent, { anim_ms = 1000, arc_sweep = 60 })`
- `lvgl.buttonmatrix(parent, { map = {"1", "2", "\n", "3"}, one_checked = true })`
- `lvgl.calendar(parent, { today = {2026, 5, 15}, shown = {2026, 5}, highlighted = {{2026, 5, 15}} })`
- `lvgl.canvas(parent, { w = 80, h = 40, color_format = "rgb565" })`
- `lvgl.chart(parent, { type = "line", point_count = 10, min = 0, max = 100, update_mode = "shift" })`
- `lvgl.imagebutton(parent, { src = "S:/path.bin" })`
- `lvgl.led(parent, { color = "#00ff00", brightness = 180, on = true })`
- `lvgl.menu(parent, opts)`
- `lvgl.msgbox(parent_or_nil, { title = "...", text = "...", buttons = {"OK"}, close_button = true })`
- `lvgl.spangroup(parent, { mode = "break", overflow = "ellipsis", spans = {"A", "B"} })`
- `lvgl.spinbox(parent, { min = 0, max = 100, value = 10, step = 1 })`
- `lvgl.tabview(parent, { tab_bar_position = "top", tab_bar_size = 36 })`
- `lvgl.tileview(parent, opts)`
- `lvgl.window(parent, opts)`

Lua index convention:
- dropdown/roller selected indexes are 1-based
- table rows and columns are 1-based
- buttonmatrix selected indexes are 1-based
- tabview active indexes are 1-based
- tileview `col` and `row` are 1-based

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

When LVGL `tiny_ttf` is enabled, fonts can be loaded from the DATA root at
runtime:

```lua
local storage = require("storage")
local lvgl = require("lvgl")

local font_path = storage.join_path(storage.get_root_dir(), "fonts/NotoSansSC-Regular.ttf")
local font = lvgl.font_load(font_path, { size = 24, cache_size = 128 })
label:set_style({ font = font })
```

- `lvgl.font_load(path, { size = px, cache_size = n })` -> font handle
- `font:set_size(px)`
- `font:is_valid()` -> boolean
- `font:delete()`

Font paths must be relative to or under the DATA root. The font file must
remain available while any LVGL object uses the font.

## Common Methods

All LVGL object userdata supports:

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
- `obj:on(event, callback)`
- `obj:off([handle_or_event])`
- `obj:delete()`
- `obj:clean()`

Common `align` names:

`top_left`, `top_mid`, `top`, `top_right`, `bottom_left`, `bottom_mid`,
`bottom`, `bottom_right`, `left_mid`, `left`, `right_mid`, `right`,
`center`, `centre`

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

## Type-Specific Methods

Basic methods:

- `label/button/checkbox/dropdown/textarea/list_text/list_button:set_text(text)`
- `bar/slider/arc/scale/dropdown/roller/checkbox/switch/spinbox:set_value(v[, anim])`
- `bar/slider/arc/scale/spinbox:get_value()`
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
- `calendar:get_pressed_date()` -> `{year, month, day}` or nil
- `canvas:fill_bg(color[, opa])`
- `canvas:set_px(x, y, color[, opa])`
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
- `menu:page(title)` -> page
- `menu:cont(parent)` -> cont
- `menu:section(page)` -> section
- `menu:separator(page)` -> separator
- `menu:set_page(page)`
- `menu:set_sidebar_page(page)`
- `menu:set_mode_header(mode)`
- `menu:set_root_back_button(bool)`
- `menu:clear_history()`
- `msgbox:add_title(text)`
- `msgbox:add_text(text)`
- `msgbox:add_footer_button(text)`
- `msgbox:add_close_button()`
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
- `tileview:add_tile(col, row, dir)` -> tile
- `tileview:set_tile(tile[, anim])`
- `tileview:set_tile_by_index(col, row[, anim])`
- `tileview:get_active_tile()` -> tile or nil
- `window:add_title(text)` -> label
- `window:add_button(icon[, width])` -> button
- `window:get_header()` -> object
- `window:get_content()` -> object
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

## Limitations

- Encoder/keypad indevs are not exposed yet.
- Image decoders and general filesystem setup are not wrapped.
- `lvgl.image(...)` and `lvgl.imagebutton(...)` only pass string `src` values
  to LVGL. Whether those strings load depends on firmware FS/decoder setup.
- Canvas support covers buffer allocation, background fill, and pixel read/write only. Advanced draw layers are not wrapped.
- Chart cursors and other advanced chart APIs are not wrapped.
- Span handles and chart series handles are not LVGL objects; they do not support object base methods such as `set_pos`, `set_style`, or `delete`.
- Non-ASCII text rendering depends on either firmware-enabled fonts or a
  runtime TTF font applied with `font`.

## Test Scripts

Directory: `components/lua_modules/lua_module_lvgl/test/`

- `lvgl_basic.lua`: basic display and widgets
- `lvgl_events.lua`: event callbacks and `process_events`
- `lvgl_indev.lua`: touch indev registration/unregistration
- `lvgl_demos.lua`: demo wrapper
- `lvgl_widgets_test.lua`: full widget test, touch-enabled when available, 60-second interactive window
