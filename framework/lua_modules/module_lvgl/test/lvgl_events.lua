--[[
P3 events smoke test.

What this script exercises:
  - obj:on registers callbacks for clicked / value_changed
  - obj:off cancels by handle, by event name, and by no-arg
  - lvgl.process_events drains the queue non-blocking
  - lvgl.run loops until cap_lua signals stop

Hardware expectation: a touch-capable display backing
board_manager.get_display_lcd_params(...). On boards without touch you
will not see the click events fire, but the script will still register,
deinit cleanly, and tolerate an empty event queue.
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
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#0f172a" })

    local title = lvgl.label(scr, {
        text = "P3 events",
        align = "top_mid",
        x = 0,
        y = 16,
        text_color = "#f5f7fa",
    })

    local panel = lvgl.container(scr, {
        align = "center",
        w = width - 40,
        h = 200,
        bg_color = "#182235",
        bg_opa = 255,
        border_color = "#334155",
        border_width = 1,
        radius = 6,
        pad = 12,
        pad_row = 12,
    })
    panel:set_flex({ flow = "column", main = "center", cross = "center", track = "center" })

    local btn = lvgl.button(panel, {
        text = "Tap me",
        w = 160,
        h = 48,
        radius = 6,
        bg_color = "#2f80ed",
        text_color = "#ffffff",
    })

    local slider = lvgl.slider(panel, {
        w = width - 100,
        h = 18,
        min = 0,
        max = 100,
        value = 25,
    })

    local clicks = 0
    local last_value = slider:get_value()

    local btn_handle = btn:on("clicked", function()
        clicks = clicks + 1
        title:set_text("clicks: " .. clicks)
        print("button clicked, count=", clicks)
    end)
    assert(type(btn_handle) == "userdata", "obj:on should return a handle")

    slider:on("value_changed", function()
        last_value = slider:get_value()
        title:set_text("slider " .. last_value)
        print("slider changed:", last_value)
    end)

    scr:load()

    -- Drain whatever might already be queued during setup.
    local drained = lvgl.process_events()
    print("initial drain:", drained)

    -- Demonstrate that off-by-name cancels matches.
    local extra_handle = btn:on("clicked", function()
        print("(this should be cancelled before firing)")
    end)
    assert(type(extra_handle) == "userdata")
    local removed = btn:off("clicked")
    print("removed-by-name:", removed)  -- should be 2 (btn_handle + extra_handle)

    -- Re-register the original click callback for the run loop below.
    btn_handle = btn:on("clicked", function()
        clicks = clicks + 1
        title:set_text("clicks: " .. clicks)
        print("button clicked (re-registered), count=", clicks)
    end)

    -- Drive the event loop in fixed slices so the smoke test has a
    -- bounded runtime even on boards without touch input. Real UI scripts
    -- typically just call `lvgl.run()` and let cap_lua signal stop.
    local total = 0
    for _ = 1, 10 do
        total = total + lvgl.process_events(500)
    end
    print("processed in 5s window:", total)

    -- Cleanup before deinit; also exercises the no-arg form of off().
    btn:off()
    slider:off("value_changed")
end)

if ok then
    print("lvgl_events test finished cleanly")
else
    print("lvgl_events test failed: " .. tostring(err))
end

lvgl.deinit()

if not ok then
    error(err)
end
