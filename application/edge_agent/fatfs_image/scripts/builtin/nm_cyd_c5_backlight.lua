--[[
NM-CYD-C5 on-board ST7789 LCD backlight (PWM brightness) quick control.

The backlight is wired to GPIO25 and driven by a 10-bit LEDC channel
configured by the board manager (`ledc_backlight` peripheral). This is
the SINGLE canonical entry to change the backlight brightness from
chat / CLI / agent. It does NOT touch the framebuffer -- only the PWM
duty cycle.

LLM AGENT GUIDE
---------------
For ANY brightness / backlight intent:

    run_lua_script script="nm_cyd_c5_backlight" args=<JSON object>

Examples:
    {percent=30}              -- dim to 30%
    {percent=100}             -- full brightness
    {percent=0}               -- backlight off
    {mode="on"}               -- shortcut for percent=100
    {mode="off"}              -- shortcut for percent=0

The script reports failures (e.g. LEDC peripheral missing) by raising
an error, so a successful return really means the duty was updated.
]]

local display = require("display")

local a = type(args) == "table" and args or {}

local mode = a.mode
if type(mode) == "string" then
    mode = string.lower(mode)
else
    mode = nil
end

local percent
if mode == "off" then
    percent = 0
elseif mode == "on" then
    percent = 100
elseif type(a.percent) == "number" then
    percent = math.floor(a.percent)
else
    error("nm_cyd_c5_backlight: provide 'percent' (0..100) or 'mode' (on/off)")
end

if percent < 0 then percent = 0 end
if percent > 100 then percent = 100 end

local ok, err = pcall(display.brightness, percent)
if not ok then
    error("nm_cyd_c5_backlight: display.brightness(" .. tostring(percent)
          .. ") failed: " .. tostring(err))
end

print(string.format("[nm_cyd_c5_backlight] set %d%%", percent))
