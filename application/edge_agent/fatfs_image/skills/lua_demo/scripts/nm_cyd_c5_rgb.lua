--[[
NM-CYD-C5 on-board WS2812 RGB LED quick control.

The on-board RGB LED is wired to GPIO27 (single pixel). This script is the
single canonical entry to drive that LED from chat / CLI / agent. Pick a
mode via the args table, the script handles RMT channel allocation and
release for you.

LLM AGENT GUIDE
---------------
Always call this script (do NOT generate ad-hoc led_strip / gpio code) :
    run_lua_script script="nm_cyd_c5_rgb" args=<table>

Common one-shot examples:
    {color="red"}                   solid red, default brightness
    {color="green", brightness=128} solid green, brightness 0..255
    {r=255, g=128, b=0}             solid orange via raw RGB
    {hue=210, sat=255, val=64}      solid HSV
    {mode="off"}                    turn the LED off and release the channel
    {mode="blink", color="blue", count=5, on_ms=200, off_ms=200}
    {mode="rainbow", duration_ms=2000, brightness=32}

Named colors: red, green, blue, white, yellow, cyan, magenta, orange,
              purple, pink, off / black.

Notes for retries: this script forces a GC at the very start, so the
common "no free tx channels" error from a previous failed run is
auto-recovered -- just run it again.
]]

-- Reclaim any RMT channel leaked by a previous failed script run.
collectgarbage("collect")

local led_strip = require("led_strip")
local delay = require("delay")

local LED_GPIO = 27
local NUM_LEDS = 1

local a = type(args) == "table" and args or {}

local function num_arg(key, default, min, max)
    local v = a[key]
    if type(v) ~= "number" then
        return default
    end
    v = math.floor(v)
    if min and v < min then v = min end
    if max and v > max then v = max end
    return v
end

local function str_arg(key, default)
    local v = a[key]
    if type(v) == "string" and #v > 0 then
        return string.lower(v)
    end
    return default
end

local NAMED_COLORS = {
    red     = {255,   0,   0},
    green   = {  0, 255,   0},
    blue    = {  0,   0, 255},
    white   = {255, 255, 255},
    yellow  = {255, 255,   0},
    cyan    = {  0, 255, 255},
    magenta = {255,   0, 255},
    orange  = {255, 128,   0},
    purple  = {128,   0, 255},
    pink    = {255,  64, 128},
    black   = {  0,   0,   0},
    off     = {  0,   0,   0},
}

local brightness = num_arg("brightness", 64, 0, 255)

local function resolve_color()
    -- Priority: explicit r/g/b > color name > hue/sat/val.
    if a.r or a.g or a.b then
        return "rgb",
               num_arg("r", 0, 0, 255),
               num_arg("g", 0, 0, 255),
               num_arg("b", 0, 0, 255)
    end
    local name = str_arg("color", nil)
    if name and NAMED_COLORS[name] then
        local c = NAMED_COLORS[name]
        local scale = brightness / 255
        return "rgb",
               math.floor(c[1] * scale),
               math.floor(c[2] * scale),
               math.floor(c[3] * scale)
    end
    if a.hue or a.sat or a.val then
        return "hsv",
               num_arg("hue", 0, 0, 359),
               num_arg("sat", 255, 0, 255),
               num_arg("val", brightness, 0, 255)
    end
    return nil
end

local mode = str_arg("mode", nil)
if not mode then
    if a.r or a.g or a.b or a.color or a.hue or a.sat or a.val then
        mode = "solid"
    else
        mode = "rainbow"   -- with no args, do a short demo sweep.
    end
end

local strip
-- When true, cleanup() will blank the LED before releasing the handle.
-- For "solid" / "on" we keep the color visible; for "off"/"blink"/"rainbow"
-- the handler itself decides whether to leave the LED lit.
local blank_on_cleanup = true

local function cleanup()
    if not strip then return end
    if blank_on_cleanup then
        pcall(function() strip:clear() end)
        pcall(function() strip:refresh() end)
    end
    pcall(function() strip:close() end)
    strip = nil
end

local function open_strip()
    strip = led_strip.new(LED_GPIO, NUM_LEDS)
end

local function set_solid(kind, x, y, z)
    if kind == "hsv" then
        for i = 0, NUM_LEDS - 1 do
            strip:set_pixel_hsv(i, x, y, z)
        end
    else
        for i = 0, NUM_LEDS - 1 do
            strip:set_pixel(i, x, y, z)
        end
    end
    strip:refresh()
end

local function run_solid()
    local kind, x, y, z = resolve_color()
    if not kind then
        kind, x, y, z = "rgb", brightness, brightness, brightness
    end
    open_strip()
    set_solid(kind, x, y, z)
    -- Keep the color showing after the script returns. WS2812 latches the
    -- last refreshed value at the LED itself, so releasing the RMT handle
    -- in cleanup() does NOT change what is displayed.
    blank_on_cleanup = false
    print(string.format("[nm_cyd_c5_rgb] solid mode=%s a=%d b=%d c=%d", kind, x, y, z))
end

local function run_off()
    open_strip()
    strip:clear()
    strip:refresh()
    print("[nm_cyd_c5_rgb] off")
end

local function run_blink()
    local kind, x, y, z = resolve_color()
    if not kind then
        kind, x, y, z = "rgb", brightness, brightness, brightness
    end
    local count  = num_arg("count",  3,   1, 100)
    local on_ms  = num_arg("on_ms",  200, 10, 5000)
    local off_ms = num_arg("off_ms", 200, 10, 5000)
    open_strip()
    for i = 1, count do
        set_solid(kind, x, y, z)
        delay.delay_ms(on_ms)
        strip:clear(); strip:refresh()
        delay.delay_ms(off_ms)
    end
    print(string.format("[nm_cyd_c5_rgb] blink count=%d", count))
end

local function run_rainbow()
    local duration_ms = num_arg("duration_ms", 2000, 100, 60000)
    local step_ms     = num_arg("step_ms",     20,   5,   1000)
    local val         = num_arg("val",         brightness, 0, 255)
    local steps = math.max(1, math.floor(duration_ms / step_ms))
    open_strip()
    for s = 0, steps - 1 do
        local hue = (s * 360 // steps) % 360
        for i = 0, NUM_LEDS - 1 do
            strip:set_pixel_hsv(i, hue, 255, val)
        end
        strip:refresh()
        delay.delay_ms(step_ms)
    end
    strip:clear(); strip:refresh()
    print(string.format("[nm_cyd_c5_rgb] rainbow duration_ms=%d", duration_ms))
end

local DISPATCH = {
    solid   = run_solid,
    on      = run_solid,
    set     = run_solid,
    color   = run_solid,
    off     = run_off,
    clear   = run_off,
    blink   = run_blink,
    flash   = run_blink,
    rainbow = run_rainbow,
    sweep   = run_rainbow,
    demo    = run_rainbow,
}

local handler = DISPATCH[mode]
if not handler then
    cleanup()
    print("[nm_cyd_c5_rgb] invalid mode: " .. tostring(mode))
    return { ok = false, error = "unknown mode", mode = tostring(mode) }
end

local ok, err = xpcall(handler, debug.traceback)
cleanup()
if not ok then
    print("[nm_cyd_c5_rgb] failed: " .. tostring(err))
    return { ok = false, error = tostring(err), mode = mode }
end

return { ok = true, mode = mode }
