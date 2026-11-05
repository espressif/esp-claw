local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end
local delay = require("delay")

local function rgb(r, g, b)
    return display.color(r, g, b)
end

local function centered_text(y, text, color, font_size)
    local w = screen_info.width
    local tw, th = screen:measure_text(text, { font_size = font_size })
    local x = math.floor((w - tw) / 2)
    screen:text(x, y, text, {
        color = color,
        font_size = font_size,
    })
    return th
end

local function wait_frame(ms)
    screen:present()
    delay.delay_ms(ms)
end

local ok, err = pcall(display.open)
if not ok then
    print("[display_demo] ERROR: init failed: " .. tostring(err))
    return
end
screen = err
screen_info = screen:info()

local screen_created = true

local function cleanup()
    if screen_created then
        pcall(screen.close, screen)
        screen_created = false
    end
end

local width = screen_info.width
local height = screen_info.height

if width <= 0 or height <= 0 then
    print("[display_demo] ERROR: invalid display size after init")
    cleanup()
    return
end

print(string.format("[display_demo] screen ready: %dx%d", width, height))
print("[display_demo] opened built-in display service")

local bg = rgb(12, 18, 28)
local fg = rgb(245, 244, 238)
local accent = rgb(255, 160, 60)
local cyan = rgb(72, 208, 235)
local green = rgb(88, 210, 124)
local red = rgb(235, 90, 90)

local run_ok, run_err = xpcall(function()
    screen:begin({ clear = bg })

    local title_h = centered_text(16, "Lua Display Demo", fg, 24)
    centered_text(16 + title_h + 6, "basic primitives + frame API", accent, 16)

    screen:stroke_rect(12, 12, width - 24, height - 24, rgb(80, 120, 160))
    screen:line(20, 72, width - 20, 72, rgb(40, 70, 95))

    screen:fill_rect(20, 92, 56, 36, cyan)
    screen:stroke_rect(20, 92, 56, 36, fg)
    screen:fill_circle(110, 110, 18, accent)
    screen:stroke_circle(110, 110, 24, fg)
    screen:line(142, 92, 196, 128, red)
    screen:line(212, 90, 252, 126, green)
    screen:line(252, 126, 220, 136, green)
    screen:line(220, 136, 212, 90, green)
    screen:fill_triangle(262, 92, 298, 126, 278, 138, rgb(110, 130, 250))

    screen:stroke_round_rect(18, 148, 92, 50, 10, rgb(255, 205, 90))
    screen:fill_round_rect(122, 148, 92, 50, 12, rgb(64, 104, 255))
    screen:stroke_circle(244, 172, 18, rgb(250, 120, 140))
    screen:fill_circle(292, 172, 12, rgb(80, 200, 150))

    screen:arc(56, 218, 18, -90, 210, rgb(255, 180, 70))
    screen:save()
    screen:translate(100, 200)
    screen:clip(0, 0, 44, 36)
    screen:fill_rect(-10, -10, 64, 56, 0x8058D27C)
    screen:restore()

    center_text(18, height - 42, width - 36, 20,
        string.format("%dx%d  frame_active=%s", width, height, tostring(true)),
        {
            color = rgb(210, 220, 228),
            font_size = 16,
        })
    wait_frame(1200)

    screen:begin({ clear = rgb(8, 10, 14) })
    for i = 0, 5 do
        local y = 20 + i * 34
        local rr = 40 + i * 30
        local gg = 120 + i * 18
        local bb = 220 - i * 22
        screen:fill_rect(18, y, width - 36, 22, rgb(rr, gg, bb))
        center_text(24, y, width - 48, 22,
            string.format("row %d  rgb(%d,%d,%d)", i + 1, rr, gg, bb),
            {
                color = "#ffffff",
                font_size = 16,
            })
    end
    wait_frame(1200)

    screen:begin({ clear = "#000000" })
    centered_text(math.floor(height / 2) - 10, "display_demo done", fg, 20)
    screen:present()
end, debug.traceback)

cleanup()
if not run_ok then
    error(run_err)
end

print("[display_demo] done")
