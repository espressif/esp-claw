local bm = require("board_manager")
local touch = require("lcd_touch")
local delay = require("delay")

local touch_handle, err = bm.get_lcd_touch_handle("lcd_touch")
if not touch_handle then
    print("[lcd_touch_demo] ERROR: get_lcd_touch_handle(lcd_touch) failed: " .. tostring(err))
    return
end

local ok, info = pcall(touch.sync, touch_handle)
if not ok then
    print("[lcd_touch_demo] ERROR: sync failed: " .. tostring(info))
    return
end

print("[lcd_touch_demo] listening for touch events for 30 seconds...")
print("[lcd_touch_demo] touch the LCD to see press / move / release events")

local last_pressed = info.pressed
local last_x = info.x or 0
local last_y = info.y or 0

for _ = 1, 600 do
    ok, info = pcall(touch.poll, touch_handle)
    if not ok then
        print("[lcd_touch_demo] ERROR: poll failed: " .. tostring(info))
        return
    end

    if info.just_pressed then
        print(string.format("[lcd_touch_demo] press    x=%d y=%d", info.x, info.y))
    elseif info.pressed and info.moved then
        print(string.format(
            "[lcd_touch_demo] move     x=%d y=%d dx=%d dy=%d held=%.0fms",
            info.x, info.y, info.dx, info.dy, info.held_ms))
    elseif info.just_released then
        print(string.format(
            "[lcd_touch_demo] release  x=%d y=%d held=%.0fms",
            info.x, info.y, info.held_ms))
    elseif info.pressed and (not last_pressed or info.x ~= last_x or info.y ~= last_y) then
        print(string.format(
            "[lcd_touch_demo] hold     x=%d y=%d held=%.0fms",
            info.x, info.y, info.held_ms))
    end

    last_pressed = info.pressed
    last_x = info.x
    last_y = info.y
    delay.delay_ms(50)
end

print("[lcd_touch_demo] done")
