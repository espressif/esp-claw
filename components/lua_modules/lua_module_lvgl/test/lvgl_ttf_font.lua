local board_manager = require("board_manager")
local delay = require("delay")
local lvgl = require("lvgl")
local storage = require("storage")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local font_path = storage.join_path(storage.get_root_dir(), "fonts/NotoSansSC-Regular.ttf")
    if not storage.exists(font_path) then
        error("missing test font: place a TTF at " .. font_path)
    end

    local font18 = lvgl.font_load(font_path, {
        size = 18,
        cache_size = 128,
    })
    local font24 = lvgl.font_load(font_path, {
        size = 24,
        cache_size = 128,
    })
    local font36 = lvgl.font_load(font_path, {
        size = 36,
        cache_size = 128,
    })

    local scr = lvgl.create_screen()
    scr:set_style({
        bg_color = "#101820",
    })

    local title = lvgl.label(scr, {
        text = "LVGL TinyTTF 中文字号",
        align = "top_mid",
        x = 0,
        y = 16,
        text_color = "#f5f7fa",
    })
    title:set_style({ font = font24 })

    local small = lvgl.label(scr, {
        text = "18px  你好，ESP-Claw",
        align = "center",
        x = 0,
        y = -48,
        text_color = "#c7d2fe",
    })
    small:set_style({ font = font18 })

    local medium = lvgl.label(scr, {
        text = "24px  你好，ESP-Claw",
        align = "center",
        x = 0,
        y = -8,
        text_color = "#ffffff",
    })
    medium:set_style({ font = font24 })

    local large = lvgl.label(scr, {
        text = "36px  你好，ESP-Claw",
        align = "center",
        x = 0,
        y = 48,
        text_color = "#fde68a",
    })
    large:set_style({ font = font36 })

    local hint = lvgl.label(scr, {
        text = "font: DATA/fonts/NotoSansSC-Regular.ttf",
        align = "bottom_mid",
        x = 0,
        y = -24,
        text_color = "#9fb3c8",
    })

    title:set_size(width - 24, 36)
    small:set_size(width - 24, 32)
    medium:set_size(width - 24, 40)
    large:set_size(width - 24, 56)
    hint:set_size(width - 24, 32)

    scr:load()
    delay.delay_ms(8000)
end)

if not ok then
    error(err)
end
