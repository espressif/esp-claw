local lvgl = require("lvgl")

lvgl.init({
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#0f172a" })

    local title = lvgl.label(scr, {
        text = "Tap the button",
        align = "top_mid",
        y = 16,
        text_color = "#f5f7fa",
    })
    local btn = lvgl.button(scr, {
        text = "Tap me",
        align = "center",
        w = 200,
        h = 64,
        bg_color = "#2f80ed",
        text_color = "#ffffff",
    })

    local taps = 0
    btn:on("clicked", function()
        taps = taps + 1
        title:set_text("taps: " .. taps)
        print("button clicked, count=", taps)
    end)

    scr:load()
    for _ = 1, 20 do
        lvgl.process_events(500)
    end
    print("automatic touch test finished, taps=", taps)
end)

lvgl.deinit()
if not ok then
    error(err)
end
