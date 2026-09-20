local display = require("display")
local delay = require("delay")

local screen, screen_info
local color_cache = {}

local function rgb(r, g, b)
    local key = (r << 16) | (g << 8) | b
    local color = color_cache[key]
    if color == nil then
        color = display.color(r, g, b)
        color_cache[key] = color
    end
    return color
end

local ok, result = pcall(display.open)
if not ok then
    print("[lcd_touch_paint] ERROR: init failed: " .. tostring(result))
    return
end
screen = result
screen_info = screen:info()

local screen_created = true
local function cleanup()
    if screen_created then
        pcall(screen.close, screen)
        screen_created = false
    end
end

local width, height = screen_info.width, screen_info.height
if width <= 0 or height <= 0 then
    print("[lcd_touch_paint] ERROR: invalid display size after init")
    cleanup()
    return
end

local TOOLBAR_H = height >= 240 and 82 or 70
local POLL_MS = 24
local CANVAS = rgb(249, 247, 240)
local TOOLBAR = rgb(28, 35, 48)
local TEXT = rgb(242, 245, 250)
local MUTED = rgb(158, 171, 190)
local ACCENT = rgb(239, 107, 83)
local BUTTON = rgb(49, 59, 75)
local BUTTON_BORDER = rgb(83, 97, 118)
local palette = {
    rgb(31, 42, 58),
    rgb(55, 126, 220),
    rgb(236, 91, 75),
    rgb(245, 178, 63),
    rgb(58, 166, 116),
    rgb(151, 97, 205),
}
local brush_sizes = { 3, 6, 10 }
local selected_color = 1
local selected_brush = 2

local button_y, button_h = 8, 28
local clear_x, clear_w = width - 126, 56
local exit_x, exit_w = width - 64, 56
local palette_y = TOOLBAR_H - 20
local palette_start = 16
local palette_end = math.max(96, width - 132)
local palette_step = math.max(16, (palette_end - palette_start) // (#palette - 1))
local brush_x = { width - 100, width - 62, width - 24 }

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end

local function inside_rect(x, y, rx, ry, rw, rh)
    return x >= rx and x < rx + rw and y >= ry and y < ry + rh
end

local function draw_toolbar()
    screen:fill_rect(0, 0, width, TOOLBAR_H, TOOLBAR)
    screen:fill_rect(0, TOOLBAR_H - 2, width, 2, ACCENT)
    screen:text(12, 8, "POCKET CANVAS", { color = TEXT, font_size = 16 })
    screen:text(12, 30, "DRAW  /  CREATE", { color = MUTED, font_size = 9 })

    screen:fill_round_rect(clear_x, button_y, clear_w, button_h, 7, BUTTON)
    screen:stroke_round_rect(clear_x, button_y, clear_w, button_h, 7, BUTTON_BORDER)
    center_text(clear_x, button_y, clear_w, button_h, "CLEAR", { color = TEXT, font_size = 11 })
    screen:fill_round_rect(exit_x, button_y, exit_w, button_h, 7, ACCENT)
    center_text(exit_x, button_y, exit_w, button_h, "EXIT", { color = rgb(255, 255, 255), font_size = 11 })

    for i, color in ipairs(palette) do
        local x = palette_start + (i - 1) * palette_step
        if i == selected_color then
            screen:fill_circle(x, palette_y, 12, TEXT)
            screen:fill_circle(x, palette_y, 10, TOOLBAR)
        end
        screen:fill_circle(x, palette_y, 8, color)
    end

    for i, radius in ipairs(brush_sizes) do
        local x = brush_x[i]
        if i == selected_brush then
            screen:fill_circle(x, palette_y, 13, ACCENT)
        end
        screen:fill_circle(x, palette_y, radius, TEXT)
    end
end

local function draw_canvas()
    screen:fill_rect(0, TOOLBAR_H, width, height - TOOLBAR_H, CANVAS)
    screen:stroke_round_rect(5, TOOLBAR_H + 5, width - 10, height - TOOLBAR_H - 10, 9, rgb(222, 218, 207))
end

local function render_all()
    screen:begin({ clear = CANVAS })
    draw_toolbar()
    draw_canvas()
    screen:present()
end

local function refresh_toolbar()
    screen:begin()
    draw_toolbar()
    screen:present()
end

local function clear_canvas()
    screen:begin()
    draw_canvas()
    screen:present()
end

local function canvas_point(x, y)
    local radius = brush_sizes[selected_brush]
    return x >= 6 + radius and x < width - 6 - radius and y >= TOOLBAR_H + 6 + radius and y < height - 6 - radius
end

local function toolbar_action(x, y)
    if inside_rect(x, y, exit_x, button_y, exit_w, button_h) then
        return "exit"
    end
    if inside_rect(x, y, clear_x, button_y, clear_w, button_h) then
        clear_canvas()
        return "control"
    end

    for i = 1, #palette do
        local px = palette_start + (i - 1) * palette_step
        local dx, dy = x - px, y - palette_y
        if dx * dx + dy * dy <= 14 * 14 then
            selected_color = i
            refresh_toolbar()
            return "control"
        end
    end

    for i = 1, #brush_sizes do
        local dx, dy = x - brush_x[i], y - palette_y
        if dx * dx + dy * dy <= 15 * 15 then
            selected_brush = i
            refresh_toolbar()
            return "control"
        end
    end
end

local function draw_segment(x0, y0, x1, y1)
    local radius = brush_sizes[selected_brush]
    local dx, dy = x1 - x0, y1 - y0
    local distance = math.max(math.abs(dx), math.abs(dy))
    local steps = math.max(1, math.ceil(distance / math.max(1, radius)))
    local color = palette[selected_color]

    screen:begin()
    for i = 0, steps do
        local x = math.floor(x0 + dx * i / steps + 0.5)
        local y = math.floor(y0 + dy * i / steps + 0.5)
        if canvas_point(x, y) then
            screen:fill_circle(x, y, radius, color)
        end
    end
    screen:present()
end

local function find_point(points, id)
    for _, point in ipairs(points) do
        if point.id == id then return point end
    end
end

render_all()
print("[lcd_touch_paint] ready; choose a color and brush, drag on the canvas, or tap EXIT")

local run_ok, run_err = xpcall(function()
    local active_id = nil
    local drawing = false
    local last_x, last_y = 0, 0

    while true do
        local points = screen:touch().points
        local point = active_id ~= nil and find_point(points, active_id) or nil

        if active_id == nil and points[1] ~= nil then
            point = points[1]
            active_id = point.id
            if point.y < TOOLBAR_H then
                local action = toolbar_action(point.x, point.y)
                if action == "exit" then return end
            elseif canvas_point(point.x, point.y) then
                drawing = true
                last_x, last_y = point.x, point.y
                draw_segment(last_x, last_y, last_x, last_y)
            end
        elseif point == nil then
            active_id = nil
            drawing = false
        elseif drawing then
            if canvas_point(point.x, point.y) then
                draw_segment(last_x, last_y, point.x, point.y)
                last_x, last_y = point.x, point.y
            else
                drawing = false
            end
        end

        delay.delay_ms(POLL_MS)
    end
end, debug.traceback)

cleanup()
if not run_ok then
    print("[lcd_touch_paint] ERROR: " .. tostring(run_err))
end
print("[lcd_touch_paint] done")
