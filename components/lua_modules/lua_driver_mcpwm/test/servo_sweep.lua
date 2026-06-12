local mcpwm = require("mcpwm")
local delay = require("delay")

local DEFAULT_GPIO = 14
local DEFAULT_FREQUENCY_HZ = 50
local DEFAULT_MIN_PULSE_US = 500
local DEFAULT_MAX_PULSE_US = 2500
local DEFAULT_START_ANGLE = 45
local DEFAULT_END_ANGLE = 135
local DEFAULT_STEP = 10
local DEFAULT_STEP_DELAY_MS = 80
local DEFAULT_EDGE_HOLD_MS = 500

local a = type(args) == "table" and args or {}
local SERVO_GPIO_NUM = type(a.gpio) == "number" and math.floor(a.gpio) or DEFAULT_GPIO
local SERVO_FREQUENCY_HZ = type(a.frequency_hz) == "number" and math.floor(a.frequency_hz) or DEFAULT_FREQUENCY_HZ
local SERVO_MIN_PULSE_US = type(a.min_pulse_us) == "number" and a.min_pulse_us or DEFAULT_MIN_PULSE_US
local SERVO_MAX_PULSE_US = type(a.max_pulse_us) == "number" and a.max_pulse_us or DEFAULT_MAX_PULSE_US
local START_ANGLE = type(a.start_angle) == "number" and a.start_angle or DEFAULT_START_ANGLE
local END_ANGLE = type(a.end_angle) == "number" and a.end_angle or DEFAULT_END_ANGLE
local STEP = type(a.step) == "number" and math.floor(math.abs(a.step)) or DEFAULT_STEP
local STEP_DELAY_MS = type(a.step_delay_ms) == "number" and math.floor(a.step_delay_ms) or DEFAULT_STEP_DELAY_MS
local EDGE_HOLD_MS = type(a.edge_hold_ms) == "number" and math.floor(a.edge_hold_ms) or DEFAULT_EDGE_HOLD_MS

local pwm

local function angle_to_duty_percent(angle)
    local clamped = angle
    if clamped < 0 then
        clamped = 0
    elseif clamped > 180 then
        clamped = 180
    end

    local pulse_width_us = SERVO_MIN_PULSE_US +
        (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * clamped / 180
    return pulse_width_us * SERVO_FREQUENCY_HZ / 10000
end

local function normalize_config()
    if STEP <= 0 then
        error("[servo] step must be > 0")
    end
    if SERVO_FREQUENCY_HZ <= 0 then
        error("[servo] frequency_hz must be > 0")
    end
    if SERVO_MIN_PULSE_US <= 0 then
        error("[servo] min_pulse_us must be > 0")
    end
    if SERVO_MAX_PULSE_US <= SERVO_MIN_PULSE_US then
        error("[servo] max_pulse_us must be > min_pulse_us")
    end
    if START_ANGLE < 0 or START_ANGLE > 180 or END_ANGLE < 0 or END_ANGLE > 180 then
        error("[servo] start_angle and end_angle must be in range 0-180")
    end
    if STEP_DELAY_MS < 0 then
        error("[servo] step_delay_ms must be >= 0")
    end
    if EDGE_HOLD_MS < 0 then
        error("[servo] edge_hold_ms must be >= 0")
    end
end

local function cleanup()
    if pwm then
        pcall(pwm.stop, pwm)
        pcall(pwm.close, pwm)
        pwm = nil
    end
end

local function move_range(from_angle, to_angle, step)
    for angle = from_angle, to_angle, step do
        pwm:set_duty(angle_to_duty_percent(angle))
        print("[servo] angle -> " .. tostring(angle))
        delay.delay_ms(STEP_DELAY_MS)
    end
end

local ok, err = xpcall(function()
    normalize_config()

    print(string.format(
        "[servo] gpio=%d freq=%d pulse_us=%s-%s range=%s->%s step=%d delay_ms=%d",
        SERVO_GPIO_NUM,
        SERVO_FREQUENCY_HZ,
        tostring(SERVO_MIN_PULSE_US),
        tostring(SERVO_MAX_PULSE_US),
        tostring(START_ANGLE),
        tostring(END_ANGLE),
        STEP,
        STEP_DELAY_MS
    ))

    pwm = mcpwm.new({
        gpio = SERVO_GPIO_NUM,
        frequency_hz = SERVO_FREQUENCY_HZ,
        duty_percent = angle_to_duty_percent(START_ANGLE),
    })

    pwm:start()
    print("[servo] pwm started")
    delay.delay_ms(EDGE_HOLD_MS)

    if START_ANGLE <= END_ANGLE then
        move_range(START_ANGLE, END_ANGLE, STEP)
        delay.delay_ms(EDGE_HOLD_MS)
        move_range(END_ANGLE, START_ANGLE, -STEP)
    else
        move_range(START_ANGLE, END_ANGLE, -STEP)
        delay.delay_ms(EDGE_HOLD_MS)
        move_range(END_ANGLE, START_ANGLE, STEP)
    end

    delay.delay_ms(EDGE_HOLD_MS)
    print("[servo] done")
end, debug.traceback)

cleanup()
if not ok then
    error(err)
end
