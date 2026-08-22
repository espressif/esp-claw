-- Color bar test for visually comparing RGB565 vs RGB888 framebuffer output.
--
-- Set the global DISPLAY_PIXEL_FORMAT (e.g. "rgb888") before requiring this
-- script to force a specific framebuffer format at display.init time. Leave it
-- nil to use the panel's default (RGB565). The screen title shows which format
-- is actually active, so you can flash the test twice and eyeball the banding
-- difference on the dark gradients.

local bm = require("board_manager")
local delay = require("delay")
local display = require("display")

local TAG = "[display_color_bars]"

local panel_handle, io_handle, width, height, panel_if, requested_format= bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " SKIP: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if, requested_format)
if not ok then
    print(TAG .. " SKIP: display.init failed: " .. tostring(err))
    return
end

local started = true
local function cleanup()
    if started then
        pcall(display.end_frame)
        pcall(display.deinit)
        started = false
    end
end

width = display.width
height = display.height
local active_format = display.pixel_format
local active_bpp = display.bytes_per_pixel

print(string.format("%s active pixel_format=%s bpp=%d size=%dx%d",
                    TAG, active_format, active_bpp, width, height))

local function draw_border(x, y, w, h)
    display.draw_rect(x - 1, y - 1, w + 2, h + 2, { r = 200, g = 200, b = 200 })
end

local function draw_bar_label(x, y, w, text)
    display.draw_text(x, y, text, {
        color = { r = 220, g = 226, b = 232 },
        font_size = 12,
    })
end

-- Horizontal gradient bar rendered via fill_rect. Each column samples the
-- gradient at value `channel_fn(v)` where v sweeps from 0 to max_val.
--
-- With max_val=255 the difference between RGB565 and RGB888 is subtle in the
-- middle of the range but shows up as slight banding in low blue/red areas.
-- With max_val<32 the 5-bit R/B channels collapse the gradient into a handful
-- of bands, making the format difference obvious at a glance.
local function draw_channel_bar(x, y, w, h, channel_fn, max_val, label)
    for i = 0, w - 1 do
        local v = math.floor(i * max_val / math.max(1, w - 1))
        display.fill_rect(x + i, y, 1, h, channel_fn(v))
    end
    draw_border(x, y, w, h)
    draw_bar_label(x, y + h + 2, w, label)
end

local function pack_rgb565(r, g, b)
    local value = math.floor(r / 8) * 2048 + math.floor(g / 4) * 32 + math.floor(b / 8)
    return string.char(value % 256, math.floor(value / 256) % 256)
end

local function pack_rgb888(r, g, b)
    return string.char(r, g, b)
end

-- Build a full-width gradient scanline in the panel's native pixel format and
-- repeat it `h` times so draw_pixels can push a single tall block. This proves
-- the draw_pixels 'format' option round-trips bytes without re-conversion.
local function make_gradient_block(w, h, channel_fn, max_val)
    local pack = (active_format == "rgb888") and pack_rgb888 or pack_rgb565
    local row_parts = {}
    for i = 0, w - 1 do
        local v = math.floor(i * max_val / math.max(1, w - 1))
        local c = channel_fn(v)
        row_parts[#row_parts + 1] = pack(c.r, c.g, c.b)
    end
    local row = table.concat(row_parts)
    return string.rep(row, h)
end

local function draw_raw_bar(x, y, w, h, channel_fn, max_val, label)
    local block = make_gradient_block(w, h, channel_fn, max_val)
    display.draw_pixels(x, y, block, {
        width = w,
        height = h,
        format = active_format,
    })
    draw_border(x, y, w, h)
    draw_bar_label(x, y + h + 2, w, label)
end

local run_ok, run_err = xpcall(function()
    display.begin_frame({ clear = true, color = { r = 8, g = 12, b = 20 } })

    display.draw_text_aligned(0, 4, width, 18,
        string.format("Color Bars  format=%s  bpp=%d", active_format, active_bpp),
        {
            color = "white",
            font_size = 14,
            align = "center",
            valign = "middle",
        })

    local bar_x = 12
    local bar_w = width - 24
    -- 7 bars + captions must fit under the title (24px) and above a bottom
    -- padding (12px). Bar height scales with the panel; caption gap is 14px.
    local bar_h = math.max(16, math.floor((height - 24 - 12 - 7 * 14) / 7))
    if bar_h > 32 then
        bar_h = 32
    end
    local gap = bar_h + 14
    local y = 26

    draw_channel_bar(bar_x, y, bar_w, bar_h,
        function(v) return { r = v, g = 0, b = 0 } end, 255, "R 0..255")
    y = y + gap

    draw_channel_bar(bar_x, y, bar_w, bar_h,
        function(v) return { r = 0, g = v, b = 0 } end, 255, "G 0..255")
    y = y + gap

    draw_channel_bar(bar_x, y, bar_w, bar_h,
        function(v) return { r = 0, g = 0, b = v } end, 255, "B 0..255")
    y = y + gap

    draw_channel_bar(bar_x, y, bar_w, bar_h,
        function(v) return { r = v, g = v, b = v } end, 255, "Gray 0..255")
    y = y + gap

    -- Dark gradients: RGB565 crushes the low nibble of red/blue so these bars
    -- degrade into ~4 visible bands, whereas RGB888 keeps a smooth ramp.
    draw_channel_bar(bar_x, y, bar_w, bar_h,
        function(v) return { r = 0, g = 0, b = v } end, 31, "Dark B 0..31 (5-bit test)")
    y = y + gap

    draw_channel_bar(bar_x, y, bar_w, bar_h,
        function(v) return { r = v, g = 0, b = 0 } end, 31, "Dark R 0..31 (5-bit test)")
    y = y + gap

    -- Native-format draw_pixels: exercises the panel-format path without the
    -- fill_rect fast path. Rendered pattern must match the earlier gray bar.
    if y + bar_h + 12 <= height then
        draw_raw_bar(bar_x, y, bar_w, bar_h,
            function(v) return { r = v, g = v, b = v } end, 255,
            string.format("Raw draw_pixels %s", active_format))
    end

    display.present()
    display.end_frame()

    delay.delay_ms(3000)

    display.begin_frame({ clear = true, color = "black" })
    display.draw_text_aligned(0, 0, width, height,
        string.format("color_bars %s PASS", active_format),
        {
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

print(TAG .. " PASS format=" .. active_format)
