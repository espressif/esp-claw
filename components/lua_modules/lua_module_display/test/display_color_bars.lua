-- Uses the default screen format; raw blit declares its source pixel format.
local delay = require("delay")
local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end

local TAG = "[display_color_bars]"

local ok, err = pcall(display.open)
if not ok then
    print(TAG .. " SKIP: display.open failed: " .. tostring(err))
    return
end
screen = err
screen_info = screen:info()

local started = true
local function cleanup()
    if started then
        pcall(screen.close, screen)
        started = false
    end
end

local width = screen_info.width
local height = screen_info.height
local active_format = screen_info.pixel_format
local active_bpp = screen_info.bytes_per_pixel

print(string.format("%s active pixel_format=%s bpp=%d size=%dx%d",
                    TAG, active_format, active_bpp, width, height))

local function draw_border(x, y, w, h)
    screen:stroke_rect(x - 1, y - 1, w + 2, h + 2, { r = 200, g = 200, b = 200 })
end

local function draw_bar_label(x, y, w, text)
    screen:text(x, y, text, {
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
        screen:fill_rect(x + i, y, 1, h, channel_fn(v))
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

-- Build source pixels in the declared blit format, independent of panel order.
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
    screen:blit(x, y, block, {
        width = w,
        height = h,
        format = active_format,
    })
    draw_border(x, y, w, h)
    draw_bar_label(x, y + h + 2, w, label)
end

local run_ok, run_err = xpcall(function()
    screen:begin({ clear = { r = 8, g = 12, b = 20 } })

    center_text(0, 4, width, 18,
        string.format("Color Bars  format=%s  bpp=%d", active_format, active_bpp),
        {
            color = "#ffffff",
            font_size = 14,
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

    -- Native-format blit: exercises the panel-format path without the
    -- fill_rect fast path. Rendered pattern must match the earlier gray bar.
    if y + bar_h + 12 <= height then
        draw_raw_bar(bar_x, y, bar_w, bar_h,
            function(v) return { r = v, g = v, b = v } end, 255,
            string.format("Raw blit %s", active_format))
    end

    screen:present()

    delay.delay_ms(3000)

    screen:begin({ clear = "#000000" })
    center_text(0, 0, width, height,
        string.format("color_bars %s PASS", active_format),
        {
            color = "#ffffff",
            font_size = 20,
        })
    screen:present()
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end

print(TAG .. " PASS format=" .. active_format)
