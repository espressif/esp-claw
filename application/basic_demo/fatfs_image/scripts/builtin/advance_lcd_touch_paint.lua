local bm = require("board_manager")
local display = require("display")
local lcd_touch = require("lcd_touch")
local delay = require("delay")

local panel_handle, io_handle, width, height, panel_if = bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print("[lcd_touch_paint] ERROR: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if)
if not ok then
    print("[lcd_touch_paint] ERROR: init failed: " .. tostring(err))
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
    print("[lcd_touch_paint] ERROR: invalid display size after init")
    cleanup()
    return
end

local BG_R, BG_G, BG_B = 250, 246, 238
local INK_R, INK_G, INK_B = 24, 74, 126
local ACCENT_R, ACCENT_G, ACCENT_B = 210, 88, 52
local TEXT_R, TEXT_G, TEXT_B = 32, 36, 42

local CLEAR_W = 74
local CLEAR_H = 32
local CLEAR_X = math.floor((width - CLEAR_W) / 2)
local CLEAR_Y = 8
local BRUSH_R = 4
local POLL_INTERVAL_MS = 10
local FLUSH_INTERVAL_MS = 16
local RUN_TIME_MS = 50000

local dirty_x0 = nil
local dirty_y0 = nil
local dirty_x1 = nil
local dirty_y1 = nil
local dirty_age_ms = 0

local function draw_ui()
    display.fill_rect(CLEAR_X, CLEAR_Y, CLEAR_W, CLEAR_H, ACCENT_R, ACCENT_G, ACCENT_B)
    display.draw_rect(CLEAR_X, CLEAR_Y, CLEAR_W, CLEAR_H, 120, 30, 12)
    display.draw_text_aligned(CLEAR_X, CLEAR_Y, CLEAR_W, CLEAR_H, "CLEAR", {
        r = 255,
        g = 255,
        b = 255,
        font_size = 16,
        align = "center",
        valign = "middle",
    })
    display.draw_text(10, 10, "LCD Touch Paint", {
        r = TEXT_R,
        g = TEXT_G,
        b = TEXT_B,
        font_size = 20,
    })
    display.draw_text(10, 34, "draw with finger, tap CLEAR to wipe", {
        r = 90,
        g = 96,
        b = 104,
        font_size = 12,
    })
    display.present()
end

local function inside_clear_button(x, y)
    return x >= CLEAR_X and x < (CLEAR_X + CLEAR_W) and y >= CLEAR_Y and y < (CLEAR_Y + CLEAR_H)
end

local function stamp_brush(x, y)
    display.fill_circle(x, y, BRUSH_R, INK_R, INK_G, INK_B)
end

local function mark_dirty(x, y, w, h)
    if w <= 0 or h <= 0 then
        return
    end

    local x0 = math.max(0, x)
    local y0 = math.max(0, y)
    local x1 = math.min(width, x + w)
    local y1 = math.min(height, y + h)
    if x1 <= x0 or y1 <= y0 then
        return
    end

    if dirty_x0 == nil then
        dirty_x0 = x0
        dirty_y0 = y0
        dirty_x1 = x1
        dirty_y1 = y1
    else
        dirty_x0 = math.min(dirty_x0, x0)
        dirty_y0 = math.min(dirty_y0, y0)
        dirty_x1 = math.max(dirty_x1, x1)
        dirty_y1 = math.max(dirty_y1, y1)
    end
end

local function mark_brush_dirty(x, y)
    mark_dirty(x - BRUSH_R - 1, y - BRUSH_R - 1, BRUSH_R * 2 + 3, BRUSH_R * 2 + 3)
end

local function draw_segment(x0, y0, x1, y1)
    display.draw_line(x0, y0, x1, y1, INK_R, INK_G, INK_B)
    stamp_brush(x1, y1)
    local min_x = math.min(x0, x1) - BRUSH_R - 1
    local min_y = math.min(y0, y1) - BRUSH_R - 1
    local max_x = math.max(x0, x1) + BRUSH_R + 1
    local max_y = math.max(y0, y1) + BRUSH_R + 1
    mark_dirty(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1)
end

local function flush_dirty(force)
    if dirty_x0 == nil then
        dirty_age_ms = 0
        return
    end
    if not force and dirty_age_ms < FLUSH_INTERVAL_MS then
        return
    end

    display.present_rect(dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0)
    dirty_x0 = nil
    dirty_y0 = nil
    dirty_x1 = nil
    dirty_y1 = nil
    dirty_age_ms = 0
end

display.begin_frame({ clear = true, r = BG_R, g = BG_G, b = BG_B })

local touch_handle, touch_err = bm.get_lcd_touch_handle("lcd_touch")
if not touch_handle then
    print("[lcd_touch_paint] ERROR: get_lcd_touch_handle(lcd_touch) failed: " .. tostring(touch_err))
    cleanup()
    return
end

local synced, info = pcall(lcd_touch.sync, touch_handle)
if not synced then
    print("[lcd_touch_paint] ERROR: lcd_touch.sync failed: " .. tostring(info))
    cleanup()
    return
end

display.clear(BG_R, BG_G, BG_B)
draw_ui()

print("[lcd_touch_paint] ready")
print("[lcd_touch_paint] drag on the LCD to paint, tap CLEAR to wipe, running for 60 seconds")

local drawing = false
local last_x = 0
local last_y = 0

for _ = 1, math.floor(RUN_TIME_MS / POLL_INTERVAL_MS) do
    local polled
    polled, info = pcall(lcd_touch.poll, touch_handle)
    if not polled then
        print("[lcd_touch_paint] ERROR: lcd_touch.poll failed: " .. tostring(info))
        break
    end

    if info.just_pressed then
        if inside_clear_button(info.x, info.y) then
            display.clear(BG_R, BG_G, BG_B)
            draw_ui()
            drawing = false
            dirty_x0 = nil
            dirty_y0 = nil
            dirty_x1 = nil
            dirty_y1 = nil
            dirty_age_ms = 0
        else
            drawing = true
            last_x = info.x
            last_y = info.y
            stamp_brush(info.x, info.y)
            mark_brush_dirty(info.x, info.y)
        end
    elseif info.pressed and drawing then
        if not inside_clear_button(info.x, info.y) then
            draw_segment(last_x, last_y, info.x, info.y)
            last_x = info.x
            last_y = info.y
        end
    elseif info.just_released then
        flush_dirty(true)
        drawing = false
    end

    dirty_age_ms = dirty_age_ms + POLL_INTERVAL_MS
    flush_dirty(false)
    delay.delay_ms(POLL_INTERVAL_MS)
end

flush_dirty(true)
cleanup()
print("[lcd_touch_paint] done")
