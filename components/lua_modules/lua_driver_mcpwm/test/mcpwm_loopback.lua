-- Connect output_gpio to a separate input_gpio with a jumper before running.
-- This checks levels and pulse frequency; use a scope for duty and edge timing.
local mcpwm = require("mcpwm")
local pcnt = require("pcnt")
local gpio = require("gpio")
local delay = require("delay")
local system = require("system")

local a = type(args) == "table" and args or {}
local output = assert(a.output_gpio, "args.output_gpio is required")
local input = assert(a.input_gpio, "args.input_gpio is required")
assert(output ~= input, "use separate output and input GPIOs")
local SAMPLE_MS = 200
local FREQUENCY_HZ = 1000
local counter, pwm

local function assert_level(expected)
    delay.delay_ms(10)
    assert(gpio.get_level(input) == expected, "unexpected physical output level")
    counter:clear()
    delay.delay_ms(SAMPLE_MS)
    assert(counter:get_count() == 0, "unexpected pulses while output should be constant")
    print(string.format("[mcpwm_loopback] level=%d, no pulses", expected))
end

local function assert_frequency(expected)
    counter:clear()
    local start_ms = system.millis()
    delay.delay_ms(SAMPLE_MS)
    local count = counter:get_count()
    local elapsed_ms = system.millis() - start_ms
    local measured = count * 1000 / elapsed_ms
    print(string.format("[mcpwm_loopback] expected=%dHz measured=%.1fHz count=%d elapsed=%dms", expected, measured, count, elapsed_ms))
    assert(math.abs(measured - expected) <= expected * 0.15,
        string.format("frequency mismatch: expected=%d measured=%.1f", expected, measured))
end

local ok, err = xpcall(function()
    counter = pcnt.new({edge_gpio = input})
    counter:start()
    for _, invert in ipairs({false, true}) do
        print(string.format("[mcpwm_loopback] output=%d input=%d invert=%s", output, input, tostring(invert)))
        pwm = mcpwm.new({gpio = output, group_id = a.group_id or 0, frequency_hz = FREQUENCY_HZ, duty_percent = 100, invert = invert})
        assert_level(0)
        pwm:start()
        assert_level(invert and 0 or 1)
        pwm:set_duty(50)
        assert_frequency(FREQUENCY_HZ)
        pwm:set_duty(0)
        assert_level(invert and 1 or 0)
        pwm:set_duty(25)
        assert_frequency(FREQUENCY_HZ)
        pwm:set_duty(100)
        pwm:stop()
        assert_level(0)
        pwm:set_duty(50)
        assert_level(0)
        pwm:start()
        assert_frequency(FREQUENCY_HZ)
        pwm:set_frequency(FREQUENCY_HZ * 2)
        assert_frequency(FREQUENCY_HZ * 2)
        pwm:set_enabled(false)
        pwm:set_frequency(FREQUENCY_HZ)
        assert_level(0)
        pwm:set_enabled(true)
        assert_frequency(FREQUENCY_HZ)
        pwm:close()
        pwm = nil
    end
    print("[mcpwm_loopback] endpoint, idle, restart, inversion and frequency checks passed")
end, debug.traceback)

if pwm then pcall(pwm.close, pwm) end
if counter then
    pcall(counter.stop, counter)
    pcall(counter.close, counter)
end
if not ok then error(err) end
