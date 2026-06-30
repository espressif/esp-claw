local bm = require("board_manager")
local delay = require("delay")
local display = require("display")

local TAG = "[display_text_style_demo]"

local function rgb(r, g, b, a)
    return { r = r, g = g, b = b, a = a or 255 }
end

local function draw_metric_row(y, label, value, color)
    display.draw_text(18, y, label, {
        color = rgb(180, 190, 205),
        font_size = 16,
    })
    display.draw_text_aligned(120, y - 1, display.width - 138, 20, value, {
        color = color,
        font_size = 16,
        align = "right",
        valign = "middle",
    })
end

local panel_handle, io_handle, width, height, panel_if = bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " SKIP: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if)
if not ok then
    print(TAG .. " SKIP: display.init failed: " .. tostring(err))
    return
end

local display_started = true

local function cleanup()
    if display_started then
        pcall(display.end_frame)
        pcall(display.deinit)
        display_started = false
    end
end

width = display.width
height = display.height

local run_ok, run_err = xpcall(function()
    local title = "Display text style demo"
    local title_w, title_h = display.measure_text(title, { font_size = 24 })
    local subtitle = string.format("measure_text: %dx%d", title_w, title_h)

    display.begin_frame({ clear = true, color = rgb(8, 12, 18) })

    display.fill_rect(0, 0, width, 48, rgb(28, 44, 64))
    display.draw_text_aligned(0, 8, width, 28, title, {
        color = "white",
        font_size = 24,
        align = "center",
        valign = "middle",
    })

    display.fill_rect(14, 66, width - 28, 104, rgb(22, 28, 36))
    display.draw_rect(14, 66, width - 28, 104, rgb(80, 110, 150))
    draw_metric_row(82, "screen", string.format("%dx%d", width, height), rgb(96, 210, 255))
    draw_metric_row(110, "title", subtitle, rgb(255, 196, 96))
    draw_metric_row(138, "frame", "active=" .. tostring(display.frame_active()), rgb(120, 230, 150))

    local box_y = math.min(height - 58, 188)
    display.fill_rect(18, box_y, width - 36, 36, rgb(255, 255, 255, 36))
    display.draw_text_aligned(18, box_y, width - 36, 36, "transparent fill + centered text", {
        color = rgb(255, 255, 255, 230),
        font_size = 16,
        bg = rgb(20, 80, 140, 120),
        align = "center",
        valign = "middle",
    })

    display.present()
    display.end_frame()
    delay.delay_ms(1400)

    display.begin_frame({ clear = false })
    display.fill_rect(18, height - 34, width - 36, 20, rgb(0, 0, 0, 180))
    display.draw_text_aligned(18, height - 34, width - 36, 20, "dirty present: footer only", {
        color = "white",
        font_size = 16,
        align = "center",
        valign = "middle",
    })
    display.present()
    display.end_frame()
    delay.delay_ms(800)
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end

print(TAG .. " PASS")
