local delay = require("delay")
local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end

local TAG = "[display_text_style_demo]"

local function rgb(r, g, b, a)
    return { r = r, g = g, b = b, a = a or 255 }
end

local function draw_metric_row(y, label, value, color)
    screen:text(18, y, label, {
        color = rgb(180, 190, 205),
        font_size = 16,
    })
    center_text(120, y - 1, screen_info.width - 138, 20, value, {
        color = color,
        font_size = 16,
    })
end

local ok, err = pcall(display.open)
if not ok then
    print(TAG .. " SKIP: display.open failed: " .. tostring(err))
    return
end
screen = err
screen_info = screen:info()

local display_started = true

local function cleanup()
    if display_started then
        pcall(screen.close, screen)
        display_started = false
    end
end

local width = screen_info.width
local height = screen_info.height

local run_ok, run_err = xpcall(function()
    local title = "Display text style demo"
    local title_w, title_h = screen:measure_text(title, { font_size = 24 })
    local subtitle = string.format("measure_text: %dx%d", title_w, title_h)

    screen:begin({ clear = rgb(8, 12, 18) })

    screen:fill_rect(0, 0, width, 48, rgb(28, 44, 64))
    center_text(0, 8, width, 28, title, {
        color = "#ffffff",
        font_size = 24,
    })

    screen:fill_rect(14, 66, width - 28, 104, rgb(22, 28, 36))
    screen:stroke_rect(14, 66, width - 28, 104, rgb(80, 110, 150))
    draw_metric_row(82, "screen", string.format("%dx%d", width, height), rgb(96, 210, 255))
    draw_metric_row(110, "title", subtitle, rgb(255, 196, 96))
    draw_metric_row(138, "frame", "active=" .. tostring(true), rgb(120, 230, 150))

    local box_y = math.min(height - 58, 188)
    screen:fill_rect(18, box_y, width - 36, 36, rgb(255, 255, 255, 36))
    center_text(18, box_y, width - 36, 36, "transparent fill + centered text", {
        color = rgb(255, 255, 255, 230),
        font_size = 16,
    })

    screen:present()
    delay.delay_ms(1400)

    screen:begin()
    screen:fill_rect(18, height - 34, width - 36, 20, rgb(0, 0, 0, 180))
    center_text(18, height - 34, width - 36, 20, "dirty present: footer only", {
        color = "#ffffff",
        font_size = 16,
    })
    screen:present()
    delay.delay_ms(800)
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end

print(TAG .. " PASS")
