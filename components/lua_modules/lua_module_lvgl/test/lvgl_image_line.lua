-- Run as an asynchronous display job; no network or image files are required.
local lvgl = require("lvgl")
local delay = require("delay")

local function rejects(fn)
    local ok = pcall(fn)
    assert(not ok, "expected an argument or stale-handle error")
end

lvgl.init()
local picture, line
local ok, err = xpcall(function()
    local screen = lvgl.create_screen()
    screen:set_style({ bg_color = "#101820", pad = 0 })
    local parent = lvgl.object(screen, { x = 20, y = 20, w = 180, h = 180, pad = 0 })
    assert(parent:add_flag("PRESS_LOCK"))
    assert(parent:clear_flag("SCROLLABLE"))
    assert(parent:clear_flag("CLICKABLE"))
    assert(parent:add_flag("CLICKABLE"))
    assert(parent:add_flag("HIDDEN"))
    assert(parent:clear_flag("HIDDEN"))
    rejects(function() parent:add_flag("UNKNOWN") end)
    rejects(function() parent:clear_flag(123) end)
    rejects(function() screen:move_foreground() end)
    assert(parent:move_foreground())
    picture = lvgl.image(parent, { x = 30, y = 30, w = 33, h = 33 })
    -- Odd width covers stride padding; dropping the string exercises buffer ownership.
    do
        local pixels = string.rep(string.char(0, 248), 33 * 33)
        assert(picture:set_raw_rgb565(33, 33, pixels))
    end
    collectgarbage("collect")
    picture:set_pivot(16, 16)
    picture:set_rotation(-450)
    screen:load()
    delay.delay_ms(40)

    rejects(function() picture:set_raw_rgb565(33, 33, "short") end)
    rejects(function() picture:set_raw_rgb565(0, 33, "") end)
    rejects(function() picture:set_raw_argb8888(16384, 1, "") end)
    rejects(function() picture:set_raw_rgb565(1 << 40, 1, "") end)
    rejects(function() picture:set_pivot(32768, 0) end)
    picture:set_rotation(450)

    line = lvgl.line(parent, { points = {{x=0,y=0}, {x=50,y=50}}, line_color = "#ffffff" })
    rejects(function() line:set_points({{x=0,y=0}, {x="bad",y=10}}) end)
    rejects(function() lvgl.line(parent, { points = {{x=0,y=0}} }) end)
    assert(line:set_points({{x=0,y=50}, {x=50,y=0}}))
    local points = {}
    for i = 1, 20 do points[i] = { x = i * 4, y = i % 2 * 40 } end
    rejects(function()
        points[20].x = "bad"
        line:set_points(points)
    end)
    points[20].x = 80
    assert(line:set_points(points))
    collectgarbage("collect")

    -- Replace formats and sizes repeatedly while the renderer consumes the source.
    for i = 1, 12 do
        assert(picture:set_raw_rgb565(33, 33, string.rep(string.char(7, 224), 33 * 33), { swapped = true }))
        delay.delay_ms(20)
        assert(picture:set_raw_argb8888(31, 31, string.rep(string.char(255, 0, 0, 160), 31 * 31)))
        picture:set_rotation(i * 300)
        line:set_points({{x=0,y=i}, {x=60,y=60-i}})
        delay.delay_ms(20)
    end
    parent:delete()
    rejects(function() parent:move_foreground() end)
    rejects(function() parent:add_flag("HIDDEN") end)
    rejects(function() picture:set_rotation(0) end)
    rejects(function() line:set_points({{x=0,y=0}, {x=10,y=10}}) end)

    picture = lvgl.image(screen, { w = 8, h = 8 })
    picture:set_raw_rgb565(8, 8, string.rep("\0", 8 * 8 * 2))
end, debug.traceback)
lvgl.deinit()
assert(ok, err)
rejects(function() picture:set_rotation(0) end)
print("lvgl image/line regression passed")
