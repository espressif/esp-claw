local bm = require("board_manager")
local display = require("display")
local delay = require("delay")

local function rgb(r, g, b)
    return { r = r, g = g, b = b }
end

local function clear_with(color)
    display.clear(color.r, color.g, color.b)
end

local function centered_text(y, text, color, font_size)
    local w = display.width()
    local tw, th = display.measure_text(text, { font_size = font_size })
    local x = math.floor((w - tw) / 2)
    display.draw_text(x, y, text, {
        r = color.r,
        g = color.g,
        b = color.b,
        font_size = font_size,
    })
    return th
end

local function wait_frame(ms)
    display.present()
    delay.delay_ms(ms)
end

local panel_handle, io_handle, width, height, panel_if = bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print("[display_demo] ERROR: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if)
if not ok then
    print("[display_demo] ERROR: init failed: " .. tostring(err))
    return
end

local screen_created = true

local function cleanup()
    if screen_created then
        pcall(display.end_frame)
        pcall(display.deinit)
        screen_created = false
    end
end

width = display.width()
height = display.height()

if width <= 0 or height <= 0 then
    print("[display_demo] ERROR: invalid display size after init")
    cleanup()
    return
end

print(string.format("[display_demo] screen ready: %dx%d", width, height))
print("[display_demo] created from board_manager handle + config")

local bg = rgb(12, 18, 28)
local fg = rgb(245, 244, 238)
local accent = rgb(255, 160, 60)
local cyan = rgb(72, 208, 235)
local green = rgb(88, 210, 124)
local red = rgb(235, 90, 90)

local run_ok, run_err = xpcall(function()
    display.begin_frame({ clear = true, r = bg.r, g = bg.g, b = bg.b })

    local title_h = centered_text(16, "Lua Display Demo", fg, 24)
    centered_text(16 + title_h + 6, "basic primitives + frame API", accent, 16)

    display.draw_rect(12, 12, width - 24, height - 24, 80, 120, 160)
    display.draw_line(20, 72, width - 20, 72, 40, 70, 95)

    display.fill_rect(20, 92, 56, 36, cyan.r, cyan.g, cyan.b)
    display.draw_rect(20, 92, 56, 36, fg.r, fg.g, fg.b)
    display.fill_circle(110, 110, 18, accent.r, accent.g, accent.b)
    display.draw_circle(110, 110, 24, fg.r, fg.g, fg.b)
    display.draw_line(142, 92, 196, 128, red.r, red.g, red.b)
    display.draw_triangle(212, 90, 252, 126, 220, 136, green.r, green.g, green.b)
    display.fill_triangle(262, 92, 298, 126, 278, 138, 110, 130, 250)

    display.draw_round_rect(18, 148, 92, 50, 10, 255, 205, 90)
    display.fill_round_rect(122, 148, 92, 50, 12, 64, 104, 255)
    display.draw_ellipse(244, 172, 34, 18, 250, 120, 140)
    display.fill_ellipse(292, 172, 20, 12, 80, 200, 150)

    display.draw_arc(56, 218, 18, -90, 210, 255, 180, 70)
    display.fill_arc(122, 218, 10, 24, -45, 225, 88, 210, 124)

    display.draw_text_aligned(18, height - 42, width - 36, 20,
        string.format("%dx%d  frame_active=%s", width, height, tostring(display.frame_active())),
        {
            r = 210,
            g = 220,
            b = 228,
            font_size = 16,
            align = "center",
            valign = "middle",
        })
    wait_frame(1200)
    display.end_frame()

    display.begin_frame({ clear = true, r = 8, g = 10, b = 14 })
    for i = 0, 5 do
        local y = 20 + i * 34
        local rr = 40 + i * 30
        local gg = 120 + i * 18
        local bb = 220 - i * 22
        display.fill_rect(18, y, width - 36, 22, rr, gg, bb)
        display.draw_text_aligned(24, y, width - 48, 22,
            string.format("row %d  rgb(%d,%d,%d)", i + 1, rr, gg, bb),
            {
                r = 255,
                g = 255,
                b = 255,
                font_size = 16,
                align = "center",
                valign = "middle",
            })
    end
    wait_frame(1200)
    display.end_frame()

    display.begin_frame({ clear = true, r = 0, g = 0, b = 0 })
    centered_text(math.floor(height / 2) - 10, "display_demo done", fg, 20)
    display.present()
    display.end_frame()
end, debug.traceback)

cleanup()
if not run_ok then
    error(run_err)
end

print("[display_demo] done")
