-- Button event demo (polling). Optional args: pin, active_level, duration_ms, poll_ms (integers).
-- Pattern: open hardware in run(), xpcall(run) + cleanup() + rethrow — copy this skeleton, not ad-hoc early returns.
local btn   = require("button")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local cfg_pin      = int_arg("pin", 0)
local active_level = int_arg("active_level", 0)
local duration_ms  = int_arg("duration_ms", 30000)
local poll_ms      = int_arg("poll_ms", 10)

local handle

local function cleanup()
    if handle then
        pcall(btn.off, handle)
        pcall(btn.close, handle)
        handle = nil
    end
end

local function run()
    local h, herr = btn.new(cfg_pin, active_level)
    if not h then
        error("[button_demo] button.new failed: " .. tostring(herr))
    end
    handle = h

    print("[button_demo] button handle created on gpio " .. tostring(cfg_pin))

    local level, lerr = btn.get_key_level(handle)
    if level == nil then
        print("[button_demo] ERROR: get_key_level: " .. tostring(lerr))
    else
        print("[button_demo] initial key level: " .. tostring(level))
    end

    btn.on(handle, "press_down", function(info)
        print("[button_demo] press_down  (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
    end)

    btn.on(handle, "press_up", function(info)
        print("[button_demo] press_up    (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
    end)

    btn.on(handle, "single_click", function()
        print("[button_demo] single_click")
    end)

    btn.on(handle, "double_click", function()
        print("[button_demo] double_click")
    end)

    btn.on(handle, "long_press_start", function(info)
        print("[button_demo] long_press_start (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
    end)

    btn.on(handle, "long_press_up", function(info)
        print("[button_demo] long_press_up   (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
    end)

    print(string.format(
        "[button_demo] listening for %d ms (poll=%dms), press the button...",
        duration_ms, poll_ms))

    local iters = math.max(1, math.floor(duration_ms / poll_ms))
    for _ = 1, iters do
        btn.dispatch()
        delay.delay_ms(poll_ms)
    end
end

local run_ok, run_err = xpcall(run, debug.traceback)
cleanup()
print("[button_demo] done")
if not run_ok then
    error(run_err)
end
