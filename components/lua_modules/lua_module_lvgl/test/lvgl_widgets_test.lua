local board_manager = require("board_manager")
local lvgl = require("lvgl")

local function check(cond, msg)
    if not cond then
        error(msg or "check failed", 2)
    end
end

local function check_eq(actual, expected, msg)
    if actual ~= expected then
        error((msg or "check_eq failed") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual), 2)
    end
end

local function section(parent, title, width)
    local box = lvgl.container(parent, {
        w = width,
        h = 92,
        bg_color = "#111827",
        bg_opa = 255,
        border_color = "#334155",
        border_width = 1,
        radius = 4,
        pad = 6,
        pad_row = 4,
    })
    box:set_flex({ flow = "column", main = "start", cross = "start" })
    lvgl.label(box, { text = title, text_color = "#e5e7eb" })
    return box
end

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local touch_registered = false

local ok, err = pcall(function()
    local test_year = 2026
    local test_month = 5
    local src_released = "S:/missing_released.bin"
    local src_pressed = "S:/missing_pressed.bin"

    local touch_handle, touch_err = board_manager.get_lcd_touch_handle("lcd_touch")
    if touch_handle == nil then
        print("no touch handle on this board, running without touch:", touch_err)
    else
        touch_registered = lvgl.indev_register("touch", touch_handle)
        print("touch indev registered:", touch_registered)
    end

    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#0f172a" })

    local root = lvgl.container(scr, {
        align = "center",
        w = width - 10,
        h = height - 10,
        bg_color = "#020617",
        bg_opa = 255,
        border_width = 0,
        pad = 6,
        pad_row = 6,
    })
    root:set_flex({ flow = "column", main = "start", cross = "start" })
    root:set_scroll({ dir = "ver", scrollbar = "auto" })

    lvgl.label(root, {
        text = "LVGL P2 full test",
        text_color = "#f8fafc",
    })

    local box = section(root, "buttonmatrix / led / spinbox", width - 24)
    local row = lvgl.container(box, { w = width - 42, h = 58, bg_opa = 0, border_width = 0, pad = 0, pad_column = 8 })
    row:set_flex({ flow = "row", main = "start", cross = "center" })

    local matrix = lvgl.buttonmatrix(row, {
        w = 120,
        h = 54,
        map = { "A", "B", "\n", "C", "D" },
        one_checked = true,
    })
    check(matrix:set_map({ "One", "Two", "\n", "Three", "Four" }), "buttonmatrix set_map")
    check(matrix:set_one_checked(true), "buttonmatrix set_one_checked")
    check(matrix:set_selected(2), "buttonmatrix set_selected")
    check_eq(matrix:get_selected(), 2, "buttonmatrix selected index")
    check_eq(matrix:get_button_text(2), "Two", "buttonmatrix selected text")

    local led = lvgl.led(row, { w = 30, h = 30, color = "#22c55e", brightness = 64, on = true })
    led:set_color("#ef4444")
    led:set_brightness(180)
    check_eq(led:get_brightness(), 180, "led brightness")
    led:off()
    led:on()
    led:toggle()
    led:toggle()

    local spin = lvgl.spinbox(row, {
        w = 92,
        h = 38,
        min = -50,
        max = 200,
        value = 10,
        step = 5,
        digit_count = 3,
        dec_point_pos = 0,
        rollover = true,
    })
    check_eq(spin:get_value(), 10, "spinbox initial value")
    spin:set_range(-100, 300)
    spin:set_step(10)
    check_eq(spin:get_step(), 10, "spinbox step")
    spin:increment()
    check_eq(spin:get_value(), 20, "spinbox increment")
    spin:decrement()
    check_eq(spin:get_value(), 10, "spinbox decrement")
    spin:step_next()
    spin:step_prev()
    spin:set_value(123)
    check_eq(spin:get_value(), 123, "spinbox set_value")

    box = section(root, "calendar / canvas", width - 24)
    local calendar = lvgl.calendar(box, {
        w = width - 42,
        h = 132,
        today = { test_year, test_month, 15 },
        shown = { test_year, test_month },
        highlighted = {
            { test_year, test_month, 15 },
            { test_year, test_month, 20 },
        },
        day_names = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" },
    })
    calendar:set_today(test_year, test_month, 16)
    calendar:set_shown(test_year, test_month)
    calendar:set_highlighted({
        { test_year, test_month, 1 },
        { test_year, test_month, 16 },
    })
    local pressed = calendar:get_pressed_date()
    check(pressed == nil or type(pressed) == "table", "calendar pressed date result")

    local canvas = lvgl.canvas(root, { w = width - 24, h = 54, color_format = "rgb565" })
    canvas:fill_bg("#000000", 255)
    canvas:set_px(2, 2, "#ff0000", 255)
    canvas:set_px(8, 6, "#00ff00", 255)
    canvas:set_px(14, 10, "#0000ff", 255)
    local px = canvas:get_px(2, 2)
    check(type(px) == "table" and type(px.r) == "number" and type(px.g) == "number" and type(px.b) == "number",
        "canvas get_px color table")

    box = section(root, "chart", width - 24)
    local chart = lvgl.chart(box, {
        w = width - 42,
        h = 74,
        type = "line",
        point_count = 6,
        min = -20,
        max = 120,
        update_mode = "shift",
    })
    local series_a = chart:add_series("#38bdf8", "primary_y")
    local series_b = chart:add_series("#f97316", "secondary_y")
    chart:set_type("bar")
    chart:set_type("line")
    chart:set_point_count(6)
    chart:set_range(0, 100)
    chart:set_range(-50, 50, "secondary_y")
    chart:set_series_values(series_a, { 10, 20, 35, 50, 75, 90 })
    chart:set_series_values(series_b, { -10, -5, 0, 5, 10, 15 })
    chart:set_next_value(series_a, 60)
    chart:set_next_value(series_b, 20)
    chart:refresh()

    box = section(root, "imagebutton / tabview", width - 24)
    local imgbtn = lvgl.imagebutton(box, {
        w = 92,
        h = 36,
        bg_color = "#1d4ed8",
        bg_opa = 255,
        src = src_released,
    })
    imgbtn:set_src("released", src_released)
    imgbtn:set_src("pressed", src_pressed)
    imgbtn:set_state("released")
    imgbtn:set_state("pressed")
    imgbtn:set_state("released")

    local tabview = lvgl.tabview(root, { w = width - 24, h = 112, tab_bar_position = "top", tab_bar_size = 30 })
    local tab_a = tabview:add_tab("Tab A")
    local tab_b = tabview:add_tab("Tab B")
    local tab_c = tabview:add_tab("Tab C")
    lvgl.label(tab_a, { text = "button " .. matrix:get_button_text(matrix:get_selected()) })
    lvgl.label(tab_b, { text = "canvas r=" .. tostring(px.r) })
    lvgl.label(tab_c, { text = "chart series ok" })
    check_eq(tabview:get_tab_count(), 3, "tabview tab count")
    tabview:set_active(2)
    check_eq(tabview:get_active(), 2, "tabview active tab")
    tabview:set_tab_text(3, "Done")

    box = section(root, "tileview / spangroup", width - 24)
    local tileview = lvgl.tileview(box, { w = width - 42, h = 62 })
    local tile_a = tileview:add_tile(1, 1, "all")
    local tile_b = tileview:add_tile(2, 1, "hor")
    lvgl.label(tile_a, { text = "Tile A", align = "center" })
    lvgl.label(tile_b, { text = "Tile B", align = "center" })
    tileview:set_tile(tile_b)
    local active_tile = tileview:get_active_tile()
    check(active_tile ~= nil, "tileview active tile")
    tileview:set_tile_by_index(1, 1)

    local spans = lvgl.spangroup(root, {
        w = width - 24,
        h = 54,
        mode = "break",
        overflow = "ellipsis",
        indent = 4,
        max_lines = 2,
        spans = { "alpha ", "beta " },
        text_color = "#e5e7eb",
    })
    local span = spans:add_span("gamma", { text_color = "#38bdf8" })
    check_eq(spans:get_span_count(), 3, "spangroup count")
    span:set_text("gamma-updated")
    check_eq(span:get_text(), "gamma-updated", "span text")
    span:set_style({ text_color = "#facc15" })
    spans:refresh()
    span:delete()
    check_eq(spans:get_span_count(), 2, "spangroup count after span delete")

    box = section(root, "menu / window / msgbox", width - 24)
    local menu = lvgl.menu(box, { w = width - 42, h = 70 })
    menu:set_mode_header("top_fixed")
    menu:set_root_back_button(true)
    local page = menu:page("Main")
    local side_page = menu:page("Side")
    local section_obj = menu:section(page)
    local cont = menu:cont(section_obj)
    lvgl.label(cont, { text = "Menu content" })
    menu:separator(page)
    menu:set_sidebar_page(side_page)
    menu:set_page(page)
    menu:clear_history()

    local win = lvgl.window(root, { w = width - 24, h = 104 })
    local title = win:add_title("Window title")
    check(title ~= nil, "window title object")
    local header_button = win:add_button(nil, 30)
    check(header_button ~= nil, "window header button")
    local header = win:get_header()
    local content = win:get_content()
    check(header ~= nil and content ~= nil, "window header/content")
    lvgl.label(content, { text = "Window content object ok" })

    local msg = lvgl.msgbox(root, {
        w = width - 24,
        title = "Msgbox",
        text = "sync close",
        buttons = { "OK", "Cancel" },
        close_button = true,
    })
    check(msg:add_title("Extra title"), "msgbox add_title")
    check(msg:add_text("Extra text"), "msgbox add_text")
    check(msg:add_footer_button("More"), "msgbox add_footer_button")
    check(msg:add_close_button(), "msgbox add_close_button")

    local async_msg = lvgl.msgbox(root, {
        w = width - 24,
        title = "Async",
        text = "close_async",
        buttons = { "OK" },
        close_button = true,
    })
    async_msg:close_async()
    msg:close()

    lvgl.label(root, {
        text = "P2 full test passed",
        text_color = "#22c55e",
    })

    scr:load()
    print("P2 full test UI is interactive for 60 seconds")
    for _ = 1, 120 do
        lvgl.process_events(500)
    end
end)

if touch_registered then
    local unreg_ok, unreg_err = pcall(lvgl.indev_unregister, "touch")
    if not unreg_ok then
        print("touch indev unregister failed: " .. tostring(unreg_err))
    end
end

local deinit_ok, deinit_err = pcall(lvgl.deinit)
if not deinit_ok then
    print("lvgl.deinit failed: " .. tostring(deinit_err))
end

if not ok then
    error(err)
end
