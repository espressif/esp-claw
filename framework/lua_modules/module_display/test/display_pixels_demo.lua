local bm = require("board_manager")
local delay = require("delay")
local display = require("display")

local TAG = "[display_pixels_demo]"
local SRC_W = 96
local SRC_H = 64

local function rgb(r, g, b)
    return { r = r, g = g, b = b }
end

local function rgb565_pixel(r, g, b)
    local value = math.floor(r / 8) * 2048 + math.floor(g / 4) * 32 + math.floor(b / 8)
    return string.char(value % 256, math.floor(value / 256) % 256)
end

local function make_pattern(width, height)
    local out = {}

    for y = 0, height - 1 do
        for x = 0, width - 1 do
            local r = math.floor(x * 255 / math.max(1, width - 1))
            local g = math.floor(y * 255 / math.max(1, height - 1))
            local b = ((math.floor(x / 8) + math.floor(y / 8)) % 2 == 0) and 220 or 40
            out[#out + 1] = rgb565_pixel(r, g, b)
        end
    end

    return table.concat(out)
end

local function draw_label(x, y, w, text)
    display.fill_rect(x, y, w, 18, { r = 0, g = 0, b = 0, a = 160 })
    display.draw_text_aligned(x, y + 1, w, 16, text, {
        color = "white",
        font_size = 12,
        align = "center",
        valign = "middle",
    })
end

local function draw_pixels_panel(x, y, w, h, label, pixels, opts)
    display.draw_rect(x - 1, y - 1, w + 2, h + 2, rgb(80, 110, 140))
    local draw_w, draw_h = display.draw_pixels(x, y, pixels, opts)
    draw_label(x, y + h + 4, w, string.format("%s %dx%d", label, draw_w, draw_h))
end

local panel_handle, io_handle, width, height, panel_if = bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " SKIP: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if)
if not ok then
    print(TAG .. " SKIP: display.init failed: " .. tostring(err))
    return
end

local display_started = true

local function cleanup()
    if display_started then
        pcall(display.end_frame)
        pcall(display.deinit)
        display_started = false
    end
end

local pixels = make_pattern(SRC_W, SRC_H)
width = display.width
height = display.height

local run_ok, run_err = xpcall(function()
    local short_ok = pcall(display.draw_pixels, 0, 0, pixels:sub(1, 16), { width = SRC_W, height = SRC_H })
    if short_ok then
        error("draw_pixels should reject short buffers")
    end

    display.begin_frame({ clear = true, color = rgb(10, 14, 22) })
    display.draw_text_aligned(0, 6, width, 20, "draw_pixels raw / fit / crop", {
        color = "white",
        font_size = 16,
        align = "center",
        valign = "middle",
    })

    local col_w = math.floor((width - 32) / 3)
    local top = 40
    local panel_h = math.min(72, math.max(44, height - 160))
    local raw_w = math.min(col_w, SRC_W)
    local raw_h = math.min(panel_h, SRC_H)

    draw_pixels_panel(8, top, col_w, panel_h, "raw", pixels, {
        width = SRC_W,
        height = SRC_H,
        mode = "raw",
        source = { x = 0, y = 0, width = raw_w, height = raw_h },
    })

    draw_pixels_panel(16 + col_w, top, col_w, panel_h, "fit", pixels, {
        width = SRC_W,
        height = SRC_H,
        mode = "fit",
        max_width = col_w,
        max_height = panel_h,
    })

    draw_pixels_panel(24 + col_w * 2, top, col_w, panel_h, "stretch", pixels, {
        width = SRC_W,
        height = SRC_H,
        mode = "stretch",
        dst_width = col_w,
        dst_height = panel_h,
    })

    local second_top = top + panel_h + 38
    local wide_w = math.min(width - 16, 220)
    local wide_h = math.min(72, math.max(40, height - second_top - 30))
    local wide_x = math.floor((width - wide_w) / 2)

    draw_pixels_panel(wide_x, second_top, wide_w, wide_h, "cover source", pixels, {
        width = SRC_W,
        height = SRC_H,
        mode = "cover",
        source = { x = 16, y = 8, width = 64, height = 48 },
        dst_width = wide_w,
        dst_height = wide_h,
    })

    display.present()
    display.end_frame()
    delay.delay_ms(1500)

    display.begin_frame({ clear = true, color = "black" })
    display.draw_text_aligned(0, 0, width, height, "display_pixels_demo PASS", {
        color = "white",
        font_size = 20,
        align = "center",
        valign = "middle",
    })
    display.present()
    display.end_frame()
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end

print(TAG .. " PASS")
