local bm = require("board_manager")
local display = require("display")
local delay = require("delay")
local system = require("system")

local FRAME_MS = 100
local RUN_TIME_MS = 180000

local BG_R, BG_G, BG_B = 6, 12, 22
local CARD_R, CARD_G, CARD_B = 10, 18, 30
local RING_R, RING_G, RING_B = 22, 38, 64
local TICK_R, TICK_G, TICK_B = 70, 118, 170
local MAJOR_TICK_R, MAJOR_TICK_G, MAJOR_TICK_B = 124, 214, 255
local HOUR_R, HOUR_G, HOUR_B = 208, 232, 255
local MIN_R, MIN_G, MIN_B = 72, 170, 255
local SEC_R, SEC_G, SEC_B = 0, 248, 208
local SHADOW_R, SHADOW_G, SHADOW_B = 2, 8, 16
local TEXT_R, TEXT_G, TEXT_B = 218, 238, 255
local SUBTEXT_R, SUBTEXT_G, SUBTEXT_B = 112, 160, 204
local GLOW_R, GLOW_G, GLOW_B = 56, 214, 255

local panel_handle, io_handle, width, height, panel_if = bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print("[clock_dial_demo] ERROR: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if)
if not ok then
    print("[clock_dial_demo] ERROR: init failed: " .. tostring(err))
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
    print("[clock_dial_demo] ERROR: invalid display size after init")
    cleanup()
    return
end

local cx = width // 2
local dial_cy = math.max(52, math.floor(height * 0.36))
local radius = math.max(44, math.min(width, math.floor(height * 0.66)) // 2 - 10)
local card_margin = 12
local card_x = card_margin
local card_y = 10
local card_w = width - card_margin * 2
local card_h = height - 20
local digital_y = math.min(height - 62, dial_cy + radius + 16)

local function polar_to_xy(center_x, center_y, r, angle_deg)
    local rad = math.rad(angle_deg - 90)
    local x = center_x + math.floor(math.cos(rad) * r + 0.5)
    local y = center_y + math.floor(math.sin(rad) * r + 0.5)
    return x, y
end

local function draw_hand(angle_deg, inner_r, outer_r, color_r, color_g, color_b)
    local x0, y0 = polar_to_xy(cx, dial_cy, inner_r, angle_deg)
    local x1, y1 = polar_to_xy(cx, dial_cy, outer_r, angle_deg)
    display.draw_line(x0, y0, x1, y1, color_r, color_g, color_b)
end

local function draw_ticks()
    for i = 0, 59 do
        local angle = i * 6
        local is_major = (i % 5) == 0
        local outer_r = radius - 8
        local inner_r = outer_r - (is_major and 14 or 7)
        local x0, y0 = polar_to_xy(cx, dial_cy, inner_r, angle)
        local x1, y1 = polar_to_xy(cx, dial_cy, outer_r, angle)
        local tick_r = is_major and MAJOR_TICK_R or TICK_R
        local tick_g = is_major and MAJOR_TICK_G or TICK_G
        local tick_b = is_major and MAJOR_TICK_B or TICK_B

        display.draw_line(x0, y0, x1, y1, tick_r, tick_g, tick_b)
    end
end

local function draw_dial_shell()
    display.clear(BG_R, BG_G, BG_B)
    display.fill_round_rect(card_x + 3, card_y + 6, card_w, card_h, 26, SHADOW_R, SHADOW_G, SHADOW_B)
    display.fill_round_rect(card_x, card_y, card_w, card_h, 26, CARD_R, CARD_G, CARD_B)
    display.draw_round_rect(card_x, card_y, card_w, card_h, 26, 34, 58, 88)
    display.draw_round_rect(card_x + 4, card_y + 4, card_w - 8, card_h - 8, 22, 18, 40, 64)

    display.fill_circle(cx, dial_cy, radius + 12, 12, 24, 40)
    display.fill_circle(cx, dial_cy, radius + 4, RING_R, RING_G, RING_B)
    display.fill_circle(cx, dial_cy, radius - 8, 8, 18, 32)
    display.fill_circle(cx, dial_cy, radius - 24, 4, 10, 20)
    display.fill_arc(cx, dial_cy, radius - 14, radius - 4, -50, 42, GLOW_R, GLOW_G, GLOW_B)
    display.fill_arc(cx, dial_cy, radius - 14, radius - 4, 132, 220, 26, 64, 108)
    display.fill_arc(cx, dial_cy, radius - 28, radius - 22, -8, 108, 32, 112, 188)
    display.fill_arc(cx, dial_cy, radius - 28, radius - 22, 172, 278, 18, 72, 130)
    draw_ticks()
end

local function parse_now()
    local h = tonumber(system.date("%H")) or 0
    local m = tonumber(system.date("%M")) or 0
    local s = tonumber(system.date("%S")) or 0
    return h, m, s
end

local function draw_time(h, m, s)
    local hour_angle = ((h % 12) + m / 60 + s / 3600) * 30
    local minute_angle = (m + s / 60) * 6
    local second_angle = s * 6
    local digital = string.format("%02d:%02d:%02d", h, m, s)
    local digital_w = display.measure_text(digital, { font_size = 28 })

    draw_hand(hour_angle, 0, radius - 56, HOUR_R, HOUR_G, HOUR_B)
    draw_hand(hour_angle + 1, 0, radius - 56, HOUR_R, HOUR_G, HOUR_B)
    draw_hand(minute_angle, 0, radius - 36, MIN_R, MIN_G, MIN_B)
    draw_hand(second_angle, 0, radius - 28, SEC_R, SEC_G, SEC_B)

    display.fill_circle(cx, dial_cy, 12, 12, 26, 44)
    display.fill_circle(cx, dial_cy, 7, SEC_R, SEC_G, SEC_B)
    display.draw_circle(cx, dial_cy, 12, 100, 190, 255)
    local sec_tip_x, sec_tip_y = polar_to_xy(cx, dial_cy, radius - 28, second_angle)
    display.fill_circle(sec_tip_x, sec_tip_y, 4, SEC_R, SEC_G, SEC_B)

    display.fill_round_rect(cx - 88, digital_y - 8, 176, 52, 16, 8, 20, 34)
    display.draw_round_rect(cx - 88, digital_y - 8, 176, 52, 16, 60, 132, 206)
    display.draw_round_rect(cx - 84, digital_y - 4, 168, 44, 13, 18, 48, 78)
    display.draw_text(cx - digital_w // 2, digital_y + 4, digital, {
        r = TEXT_R,
        g = TEXT_G,
        b = TEXT_B,
        font_size = 28,
        bg_r = 8,
        bg_g = 20,
        bg_b = 34,
    })
end

local run_ok, run_err = xpcall(function()
    local elapsed_ms = 0

    while elapsed_ms < RUN_TIME_MS do
        local hour, minute, second = parse_now()

        display.begin_frame({ clear = true, r = BG_R, g = BG_G, b = BG_B })
        draw_dial_shell()
        draw_time(hour, minute, second)
        display.present()
        display.end_frame()

        delay.delay_ms(FRAME_MS)
        elapsed_ms = elapsed_ms + FRAME_MS
    end
end, debug.traceback)

cleanup()
if not run_ok then
    error(run_err)
end

print("[clock_dial_demo] done")
