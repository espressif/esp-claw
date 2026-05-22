--[[
NM-CYD-C5 on-board ST7789 LCD quick control.

This is the SINGLE canonical screen entry. It correctly:
  1. acquires the display via the board_manager arbiter,
  2. begins a frame, paints the requested content, presents it,
  3. waits for `duration_ms` (sync) so the picture is actually visible,
  4. calls end_frame + display.deinit so emote ownership is restored.

LLM AGENT GUIDE
---------------
For ANY "set screen / show on screen / display X / 屏幕显示" request:

    run_lua_script script="nm_cyd_c5_screen" args=<JSON object>

Do NOT call `lua_run_script_async` -- the duration must be a synchronous
wait inside this script, otherwise the script returns immediately and
the emote app reclaims the screen before the user sees anything.

### Modes
- `color`   solid color fill. Fields:
              `color` ("red", "green", ...) OR `r`,`g`,`b` (0..255)
              `duration_ms` default 3000
- `text`    centered text on a colored background. Fields:
              `text` (required, string),
              `bg`   color name or {r,g,b}, default black,
              `fg`   color name or {r,g,b}, default white,
              `font_size` default 28,
              `duration_ms` default 5000
- `message` like `text` but with a small title bar. Fields:
              `title` (string), `text` (string), plus bg/fg/duration_ms
- `clear`   black screen for `duration_ms` (default 1000), then release.

### Examples
- 红屏 10 秒          : {"mode":"color","color":"red","duration_ms":10000}
- 绿色 5 秒           : {"mode":"color","color":"green","duration_ms":5000}
- 自定义 RGB 3 秒     : {"r":50,"g":50,"b":200,"duration_ms":3000}
- 居中显示文字       : {"mode":"text","text":"Hello!","bg":"blue"}
- 标题 + 内容        : {"mode":"message","title":"Status","text":"WiFi OK"}
- 关闭/清屏          : {"mode":"clear"}

Named colors: red, green, blue, white, black, yellow, cyan, magenta,
              orange, purple, pink, gray.
]]

local bm = require("board_manager")
local display = require("display")
local delay = require("delay")

local a = type(args) == "table" and args or {}

-- ---------------------------------------------------------------- helpers
local function num_arg(key, default, min, max)
    local v = a[key]
    if type(v) ~= "number" then return default end
    v = math.floor(v)
    if min and v < min then v = min end
    if max and v > max then v = max end
    return v
end

local function str_arg(key, default)
    local v = a[key]
    if type(v) == "string" and #v > 0 then return string.lower(v) end
    return default
end

local NAMED_COLORS = {
    red     = {255,   0,   0},
    green   = {  0, 200,   0},
    blue    = {  0,   0, 255},
    white   = {255, 255, 255},
    black   = {  0,   0,   0},
    yellow  = {255, 255,   0},
    cyan    = {  0, 200, 235},
    magenta = {200,   0, 200},
    orange  = {255, 140,   0},
    purple  = {128,   0, 200},
    pink    = {255,  90, 150},
    gray    = {128, 128, 128},
}

local function parse_color(value, default)
    if type(value) == "string" then
        local c = NAMED_COLORS[string.lower(value)]
        if c then return c[1], c[2], c[3] end
    elseif type(value) == "table" then
        local r = type(value.r) == "number" and value.r or value[1]
        local g = type(value.g) == "number" and value.g or value[2]
        local b = type(value.b) == "number" and value.b or value[3]
        if type(r) == "number" and type(g) == "number" and type(b) == "number" then
            return math.floor(r), math.floor(g), math.floor(b)
        end
    end
    return default[1], default[2], default[3]
end

-- Mode resolution: if `color`, `r/g/b` is given but no `mode`, default to color.
local mode = str_arg("mode", nil)
if not mode then
    if a.color or a.r or a.g or a.b then
        mode = "color"
    elseif a.title or a.text then
        mode = a.title and "message" or "text"
    else
        mode = "color"
    end
end

-- ---------------------------------------------- acquire & release display
local owned = false
local frame_open = false

local function close_display()
    if frame_open then
        pcall(display.end_frame)
        frame_open = false
    end
    if owned then
        pcall(display.deinit)
        owned = false
    end
end

local function open_display()
    local panel_handle, io_handle, w, h, panel_if =
        bm.get_display_lcd_params("display_lcd")
    if not panel_handle then
        error("nm_cyd_c5_screen: get_display_lcd_params failed: " ..
              tostring(io_handle))
    end
    local ok, err = pcall(display.init, panel_handle, io_handle, w, h, panel_if)
    if not ok then
        error("nm_cyd_c5_screen: display.init failed: " .. tostring(err))
    end
    owned = true
    return display.width(), display.height()
end

-- ------------------------------------------------------------ mode handlers
local function fill_solid(r, g, b)
    display.begin_frame({ clear = true, r = r, g = g, b = b })
    frame_open = true
    display.present()
end

local function run_color()
    local default_color = NAMED_COLORS.red
    local r, g, b
    if a.r or a.g or a.b then
        r = num_arg("r", default_color[1], 0, 255)
        g = num_arg("g", default_color[2], 0, 255)
        b = num_arg("b", default_color[3], 0, 255)
    else
        r, g, b = parse_color(a.color or str_arg("color", "red"), default_color)
    end
    open_display()
    fill_solid(r, g, b)
    local duration = num_arg("duration_ms", 3000, 0, 60000)
    if duration > 0 then delay.delay_ms(duration) end
    print(string.format("[nm_cyd_c5_screen] color rgb=(%d,%d,%d) %dms",
                        r, g, b, duration))
end

local function center_text(text, font_size, fr, fg, fb, w, h)
    local tw, th = display.measure_text(text, { font_size = font_size })
    local x = math.floor((w - tw) / 2)
    local y = math.floor((h - th) / 2)
    display.draw_text(x, y, text, { r = fr, g = fg, b = fb, font_size = font_size })
end

local function run_text()
    local text = a.text
    if type(text) ~= "string" or #text == 0 then
        error("nm_cyd_c5_screen: 'text' field is required for mode=text")
    end
    local br, bg, bb = parse_color(a.bg, NAMED_COLORS.black)
    local fr, fg, fb = parse_color(a.fg, NAMED_COLORS.white)
    local font_size = num_arg("font_size", 28, 8, 96)
    local w, h = open_display()
    display.begin_frame({ clear = true, r = br, g = bg, b = bb })
    frame_open = true
    center_text(text, font_size, fr, fg, fb, w, h)
    display.present()
    local duration = num_arg("duration_ms", 5000, 0, 60000)
    if duration > 0 then delay.delay_ms(duration) end
    print(string.format("[nm_cyd_c5_screen] text \"%s\" %dms", text, duration))
end

local function run_message()
    local title = a.title or ""
    local text = a.text or ""
    if (type(title) ~= "string" or #title == 0) and
       (type(text)  ~= "string" or #text  == 0) then
        error("nm_cyd_c5_screen: provide 'title' and/or 'text' for mode=message")
    end
    local br, bgc, bb = parse_color(a.bg, NAMED_COLORS.black)
    local fr, fgc, fb = parse_color(a.fg, NAMED_COLORS.white)
    local title_size = num_arg("title_font_size", 20, 8, 96)
    local body_size  = num_arg("font_size", 24, 8, 96)

    local w, h = open_display()
    display.begin_frame({ clear = true, r = br, g = bgc, b = bb })
    frame_open = true

    -- Title bar across the top (1/5 of height, capped at 60 px).
    local bar_h = math.min(60, math.floor(h / 5))
    -- Slightly lighter accent bar.
    local ar, ag, ab = math.min(255, br + 40), math.min(255, bgc + 40),
                       math.min(255, bb + 40)
    display.fill_rect(0, 0, w, bar_h, ar, ag, ab)
    if #title > 0 then
        display.draw_text_aligned(0, 0, w, bar_h, title,
            { r = fr, g = fgc, b = fb, font_size = title_size,
              align = "center", valign = "middle" })
    end

    if #text > 0 then
        local body_y = bar_h
        local body_h = h - bar_h
        display.draw_text_aligned(8, body_y, w - 16, body_h, text,
            { r = fr, g = fgc, b = fb, font_size = body_size,
              align = "center", valign = "middle" })
    end

    display.present()
    local duration = num_arg("duration_ms", 5000, 0, 60000)
    if duration > 0 then delay.delay_ms(duration) end
    print(string.format("[nm_cyd_c5_screen] message title=\"%s\" %dms",
                        title, duration))
end

local function run_clear()
    open_display()
    fill_solid(0, 0, 0)
    local duration = num_arg("duration_ms", 1000, 0, 60000)
    if duration > 0 then delay.delay_ms(duration) end
    print("[nm_cyd_c5_screen] clear")
end

local DISPATCH = {
    color   = run_color,
    fill    = run_color,
    bg      = run_color,
    text    = run_text,
    label   = run_text,
    message = run_message,
    msg     = run_message,
    notify  = run_message,
    clear   = run_clear,
    off     = run_clear,
    blank   = run_clear,
}

local handler = DISPATCH[mode]
if not handler then
    print("[nm_cyd_c5_screen] invalid mode: " .. tostring(mode))
    return { ok = false, error = "unknown mode", mode = tostring(mode) }
end

local ok, err = xpcall(handler, debug.traceback)
close_display()
if not ok then
    print("[nm_cyd_c5_screen] failed: " .. tostring(err))
    return { ok = false, error = tostring(err), mode = mode }
end

return { ok = true, mode = mode }
