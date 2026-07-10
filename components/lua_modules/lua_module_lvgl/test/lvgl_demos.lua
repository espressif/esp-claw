--[[
LVGL demo wrapper smoke test.

What this script exercises:
  - lvgl.demos() reports compiled-in runnable demo names before init
  - lvgl.demo(name) rejects calls before the LVGL runtime is initialized
  - lvgl.demo(name) runs an available no-argument demo after init
  - unavailable demo names are rejected
]]

local board_manager = require("board_manager")
local delay = require("delay")
local lvgl = require("lvgl")

local function contains(list, value)
    for _, item in ipairs(list) do
        if item == value then
            return true
        end
    end
    return false
end

local demos = lvgl.demos()
assert(type(demos) == "table", "lvgl.demos() should return a table")

print("compiled lvgl demos:")
for i, name in ipairs(demos) do
    assert(type(name) == "string", "demo name should be a string")
    print(i, name)
end

if #demos > 0 then
    local preinit_ok, preinit_err = pcall(lvgl.demo, demos[1])
    assert(not preinit_ok, "lvgl.demo should reject calls before lvgl.init")
    assert(string.find(tostring(preinit_err), "runtime is not initialized", 1, true),
           "pre-init lvgl.demo error should mention runtime initialization")
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
    local touch_handle, touch_err = board_manager.get_lcd_touch_handle("lcd_touch")
    if touch_handle == nil then
        print("no touch handle on this board, demo will run without touch:", touch_err)
    else
        touch_registered = lvgl.indev_register("touch", touch_handle)
        print("touch indev registered:", touch_registered)
    end

    local bad_ok, bad_err = pcall(lvgl.demo, "__missing_demo__")
    assert(not bad_ok, "unknown demo should be rejected")
    assert(string.find(tostring(bad_err), "demo unavailable", 1, true),
           "unknown demo error should mention demo unavailable")

    if contains(demos, "music") then
        print("music demo is compiled in")
    else
        local music_ok, music_err = pcall(lvgl.demo, "music")
        assert(not music_ok, "disabled music demo should be rejected")
        assert(string.find(tostring(music_err), "demo unavailable", 1, true),
               "disabled music demo error should mention demo unavailable")
    end

    if #demos == 0 then
        print("no compiled lvgl demos, skipping demo run")
        return
    end

    assert(lvgl.demo(demos[1]) == true, "lvgl.demo should return true")
    print("ran lvgl demo:", demos[1])
    delay.delay_ms(3000)
end)

if touch_registered then
    lvgl.indev_unregister("touch")
end
lvgl.deinit()

if ok then
    print("lvgl_demos test finished cleanly")
else
    print("lvgl_demos test failed: " .. tostring(err))
    error(err)
end
