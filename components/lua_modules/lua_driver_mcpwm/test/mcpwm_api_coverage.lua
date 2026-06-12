-- MCPWM API coverage test. Use args.gpio_a/gpio_b to select safe pins for
-- the target board before running this on real hardware.
local mcpwm = require("mcpwm")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local GPIO_A = type(a.gpio_a) == "number" and math.floor(a.gpio_a)
    or type(a.gpio) == "number" and math.floor(a.gpio)
    or 4
local GPIO_B = type(a.gpio_b) == "number" and math.floor(a.gpio_b) or 5
local GROUP_ID = type(a.group_id) == "number" and math.floor(a.group_id) or 0
local FREQUENCY_HZ = type(a.frequency_hz) == "number" and math.floor(a.frequency_hz) or 1000
local NEW_FREQUENCY_HZ = type(a.new_frequency_hz) == "number" and math.floor(a.new_frequency_hz) or 2000
local RESOLUTION_HZ = type(a.resolution_hz) == "number" and math.floor(a.resolution_hz) or 1000000
local HOLD_MS = type(a.hold_ms) == "number" and math.floor(a.hold_ms) or 100

local handles = {}

local function expect_error(name, fn)
    local ok, err = pcall(fn)
    if ok then
        error("[mcpwm_api] expected error: " .. name)
    end
    print(string.format("[mcpwm_api] expected error OK: %s (%s)", name, tostring(err)))
end

local function assert_equal(name, actual, expected)
    if actual ~= expected then
        error(string.format(
            "[mcpwm_api] %s failed: expected=%s actual=%s",
            name,
            tostring(expected),
            tostring(actual)
        ))
    end
    print(string.format("[mcpwm_api] %s OK", name))
end

local function track(handle)
    handles[#handles + 1] = handle
    return handle
end

local function cleanup()
    for _, handle in ipairs(handles) do
        pcall(handle.stop, handle)
        pcall(handle.close, handle)
    end
    handles = {}
end

local function normalize()
    if GPIO_A == GPIO_B then
        error("[mcpwm_api] gpio_a and gpio_b must be different")
    end
    if GROUP_ID < 0 then
        error("[mcpwm_api] group_id must be >= 0")
    end
    if FREQUENCY_HZ <= 0 or NEW_FREQUENCY_HZ <= 0 then
        error("[mcpwm_api] frequency values must be > 0")
    end
    if RESOLUTION_HZ < FREQUENCY_HZ or RESOLUTION_HZ < NEW_FREQUENCY_HZ then
        error("[mcpwm_api] resolution_hz must be >= tested frequencies")
    end
    if HOLD_MS < 0 then
        error("[mcpwm_api] hold_ms must be >= 0")
    end
end

local ok, err = xpcall(function()
    normalize()

    print(string.format(
        "[mcpwm_api] gpio_a=%d gpio_b=%d group=%d freq=%d new_freq=%d resolution=%d",
        GPIO_A,
        GPIO_B,
        GROUP_ID,
        FREQUENCY_HZ,
        NEW_FREQUENCY_HZ,
        RESOLUTION_HZ
    ))

    expect_error("mcpwm.new missing gpio", function()
        mcpwm.new({})
    end)
    expect_error("mcpwm.new invalid group_id", function()
        mcpwm.new({gpio = GPIO_A, group_id = -1})
    end)
    expect_error("mcpwm.new invalid frequency", function()
        mcpwm.new({gpio = GPIO_A, frequency_hz = 0})
    end)
    expect_error("mcpwm.new invalid resolution", function()
        mcpwm.new({gpio = GPIO_A, resolution_hz = 1000, frequency_hz = 2000})
    end)
    expect_error("mcpwm.new invalid duty", function()
        mcpwm.new({gpio = GPIO_A, duty_percent = 101})
    end)
    expect_error("mcpwm.new duplicate GPIO", function()
        mcpwm.new({gpio = GPIO_A, gpio_b = GPIO_A})
    end)

    local default_pwm = track(mcpwm.new({gpio = GPIO_A}))
    assert_equal("default get_channel_count", default_pwm:get_channel_count(), 1)
    default_pwm:start()
    default_pwm:stop()
    default_pwm:close()
    print("[mcpwm_api] mcpwm.new default config OK")

    local pwm = track(mcpwm.new({
        gpio_a = GPIO_A,
        gpio_b = GPIO_B,
        group_id = GROUP_ID,
        resolution_hz = RESOLUTION_HZ,
        frequency_hz = FREQUENCY_HZ,
        duty_percent = 25,
        duty_percent_b = 75,
        invert = false,
        invert_b = true,
    }))
    assert_equal("dual get_channel_count", pwm:get_channel_count(), 2)

    pwm:start()
    pwm:start()
    print("[mcpwm_api] start idempotent OK")
    delay.delay_ms(HOLD_MS)

    pwm:set_duty(0)
    pwm:set_duty(100)
    pwm:set_duty(50)
    print("[mcpwm_api] set_duty(percent) including 0/100 OK")

    pwm:set_duty(1, 20)
    pwm:set_duty(2, 80)
    print("[mcpwm_api] set_duty(channel, percent) OK")

    expect_error("set_duty invalid low duty", function()
        pwm:set_duty(-1)
    end)
    expect_error("set_duty invalid high duty", function()
        pwm:set_duty(101)
    end)
    expect_error("set_duty invalid channel", function()
        pwm:set_duty(3, 50)
    end)

    pwm:set_frequency(NEW_FREQUENCY_HZ)
    print("[mcpwm_api] set_frequency OK")
    expect_error("set_frequency zero", function()
        pwm:set_frequency(0)
    end)
    expect_error("set_frequency above resolution", function()
        pwm:set_frequency(RESOLUTION_HZ + 1)
    end)

    pwm:set_enabled(false)
    pwm:set_enabled(false)
    pwm:set_enabled(true)
    pwm:set_enabled(true)
    print("[mcpwm_api] set_enabled true/false idempotent OK")
    delay.delay_ms(HOLD_MS)

    pwm:stop()
    pwm:stop()
    print("[mcpwm_api] stop idempotent OK")
    pwm:close()
    pwm:close()
    print("[mcpwm_api] close idempotent OK")

    expect_error("closed handle access", function()
        pwm:get_channel_count()
    end)

    print("[mcpwm_api] done")
end, debug.traceback)

cleanup()
if not ok then
    error(err)
end
