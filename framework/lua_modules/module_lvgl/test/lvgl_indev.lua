--[[
P4 indev (touch) smoke test.

What this script exercises:
  - lvgl.indev_register("touch", touch_handle) attaches a pointer indev
  - LVGL events ("clicked" / "pressed") are reachable on touch input
  - lvgl.indev_unregister("touch") detaches cleanly without affecting the
    underlying esp_lcd_touch handle

Hardware expectation:
  - board_manager exposes a touch handle under "display_touch" (typical
    on display-equipped dev boards). On boards without touch the script
    skips the indev step and still verifies setup / teardown.
]]

local board_manager = require("board_manager")
local lvgl = require("lvgl")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local touch_handle, terr = board_manager.get_lcd_touch_handle("lcd_touch")
    if touch_handle == nil then
        print("no touch handle on this board, skipping indev test:", terr)
    else
        local registered = lvgl.indev_register("touch", touch_handle)
        assert(registered == true, "indev_register should return true")
        print("touch indev registered ok")

        local dup_ok, dup_err = pcall(lvgl.indev_register, "touch", touch_handle)
        assert(not dup_ok, "double-register must error")
        print("re-register correctly rejected:", dup_err)
    end

    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#0f172a" })

    local title = lvgl.label(scr, {
        text = "Tap anywhere",
        align = "top_mid",
        x = 0,
        y = 16,
        text_color = "#f5f7fa",
    })

    local btn = lvgl.button(scr, {
        text = "Tap me",
        align = "center",
        w = 200,
        h = 64,
        radius = 8,
        bg_color = "#2f80ed",
        text_color = "#ffffff",
    })

    local taps = 0
    btn:on("clicked", function()
        taps = taps + 1
        title:set_text("taps: " .. taps)
        print("button clicked, count=", taps)
    end)

    local presses = 0
    scr:on("pressed", function()
        presses = presses + 1
        print("screen pressed, count=", presses)
    end)

    scr:load()

    -- Bounded run window so the smoke test exits even when no one taps
    -- the panel. ~5s of touch-friendly event processing.
    local total = 0
    for _ = 1, 10 do
        total = total + lvgl.process_events(500)
    end
    print("events processed:", total, "taps:", taps, "presses:", presses)

    btn:off()
    scr:off()

    if touch_handle ~= nil then
        local removed = lvgl.indev_unregister("touch")
        assert(removed == true, "indev_unregister should report removal")
        print("touch indev unregistered ok")
    end

    -- Calling indev_unregister twice should report no-op rather than error.
    local second = lvgl.indev_unregister("touch")
    print("second unregister returned:", second)
end)

if ok then
    print("lvgl_indev test finished cleanly")
else
    print("lvgl_indev test failed: " .. tostring(err))
end

lvgl.deinit()

if not ok then
    error(err)
end
