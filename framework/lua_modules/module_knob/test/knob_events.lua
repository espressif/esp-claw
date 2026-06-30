-- Rotary encoder (knob) demo (polling).
-- Optional args: gpio_a, gpio_b, direction, duration_ms, poll_ms (integers).
-- Pattern: open hardware in run(), xpcall(run) + cleanup() + rethrow.
local knob  = require("knob")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local cfg_gpio_a   = int_arg("gpio_a", 48)
local cfg_gpio_b   = int_arg("gpio_b", 47)
local cfg_dir      = int_arg("direction", 0)
local duration_ms  = int_arg("duration_ms", 30000)
local poll_ms      = int_arg("poll_ms", 10)

local handle

local function cleanup()
    if handle then
        pcall(knob.off, handle)
        pcall(knob.close, handle)
        handle = nil
    end
end

local function run()
    local h, herr = knob.new(cfg_gpio_a, cfg_gpio_b, cfg_dir)
    if not h then
        error("[knob_demo] knob.new failed: " .. tostring(herr))
    end
    handle = h

    print("[knob_demo] knob created on gpio_a=" .. tostring(cfg_gpio_a)
          .. " gpio_b=" .. tostring(cfg_gpio_b))

    knob.on(handle, "left", function(evt)
        print("[knob_demo] left   count=" .. tostring(evt.count))
    end)

    knob.on(handle, "right", function(evt)
        print("[knob_demo] right  count=" .. tostring(evt.count))
    end)

    knob.on(handle, "zero", function(evt)
        print("[knob_demo] zero   count=" .. tostring(evt.count))
    end)

    print(string.format(
        "[knob_demo] listening for %d ms (poll=%dms), rotate the knob...",
        duration_ms, poll_ms))

    local iters = math.max(1, math.floor(duration_ms / poll_ms))
    for _ = 1, iters do
        knob.dispatch()
        delay.delay_ms(poll_ms)
    end
end

local run_ok, run_err = xpcall(run, debug.traceback)
cleanup()
print("[knob_demo] done")
if not run_ok then
    error(run_err)
end
