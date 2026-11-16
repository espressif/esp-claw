-- --------------------------------------------------------------
-- StackChan environment and power sensors.
--
-- Two chips that are driven from Lua and therefore do not appear in
-- board_devices.yaml: the LTR-553ALS ambient light / proximity sensor
-- on the CoreS3 itself (I2C 0x23), and the INA226 battery monitor on
-- the Power board (I2C 0x41, 10 mOhm shunt).
--
-- Board wiring and the INA226's shunt value live here rather than in
-- the drivers, because they are properties of this board. The drivers
-- stay board-agnostic.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local ltr553 = require("lib_ltr553")
local ina226 = require("lib_ina226")
local i2c = require("i2c")
local delay = require("delay")

-- 2. Constants
local I2C_PORT = 0
local I2C_SDA = 12
local I2C_SCL = 11
local I2C_FREQ_HZ = 400000

local LIGHT_ADDR = 0x23
local POWER_ADDR = 0x41
-- Measured from the POWER board schematic; wrong values here would scale every
-- current reading by a constant without raising an error.
local SHUNT_OHMS = 0.01
local MAX_EXPECTED_CURRENT_A = 8.19

-- Current is positive while discharging on this board, confirmed on hardware by
-- watching the sign flip across a USB plug/unplug cycle. Below this magnitude
-- the reading is noise rather than a direction.
local IDLE_CURRENT_A = 0.02

-- 3. Args
local ACTIONS = {all = true, light = true, power = true}

local ctx = arg_schema.parse(args, {
    samples = arg_schema.int({default = 1, min = 1, max = 20}),
    interval_ms = arg_schema.int({default = 300, min = 0, max = 5000}),
    proximity = arg_schema.bool({default = false}),
})

local raw_args = type(args) == "table" and args or {}
local ACTION = "all"
if raw_args.action ~= nil then
    if type(raw_args.action) ~= "string" or not ACTIONS[raw_args.action] then
        error("action must be one of: all, light, power")
    end
    ACTION = raw_args.action
end

-- 4. Cleanup
local bus, light, power

local function close_one(name, obj)
    if obj then
        local ok, err = pcall(function() obj:close() end)
        if not ok then
            print(string.format("[sensors] WARN: closing %s failed: %s", name, tostring(err)))
        end
    end
end

local function cleanup()
    close_one("light sensor", light)
    close_one("power monitor", power)
    light, power = nil, nil
    if bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

-- 5. Interpretation
-- A lux number alone does not answer "is it dark in here", and the model should
-- not have to invent thresholds. These bands are ordinary indoor lighting
-- references, not a calibration: this part reports relative light well but its
-- absolute scale has not been calibrated on this board.
local LIGHT_BANDS = {
    {10, "dark (unlit room, or the sensor is covered)"},
    {50, "dim (evening indoors, curtains drawn)"},
    {200, "normal indoor lighting"},
    {1000, "bright (well-lit room or near a window)"},
    {math.huge, "very bright (direct daylight)"},
}

local function light_band(lux)
    for _, band in ipairs(LIGHT_BANDS) do
        if lux < band[1] then
            return band[2]
        end
    end
    return "unknown"
end

local function charge_state(current)
    if current > IDLE_CURRENT_A then
        return "discharging (running on battery)"
    end
    if current < -IDLE_CURRENT_A then
        return "charging"
    end
    return "idle (no meaningful current either way)"
end

-- 6. Readings
local function report_light()
    local sample = light:read()
    print(string.format("[sensors] light %.1f lux -- %s", sample.lux, light_band(sample.lux)))
    print(string.format("[sensors]   raw ch0=%d ch1=%d, gain x%d, integration %d ms",
        sample.ch0, sample.ch1, light:gain(), light:integration_time_ms()))
    if sample.data_invalid then
        print("[sensors]   WARNING: the sensor flagged this sample invalid")
    end
    if sample.proximity then
        print(string.format("[sensors]   proximity %d%s", sample.proximity,
            sample.proximity_saturated and " (saturated: something is very close)" or ""))
    end
end

local function report_power()
    local sample = power:read()
    print(string.format("[sensors] battery %.2f V, %.0f mA, %.2f W -- %s",
        sample.bus_voltage, sample.current * 1000, sample.power,
        charge_state(sample.current)))
    print("[sensors]   current is positive while discharging on this board")
end

-- 7. Run
-- One chip failing must not hide the other: a dead light sensor should still let
-- the battery be read, and each failure is reported with the address so it can
-- be told apart from a whole-bus problem.
local function open_light()
    light = ltr553.new({bus = bus, addr = LIGHT_ADDR, enable_ps = ctx.proximity})
end

local function open_power()
    power = ina226.new({
        bus = bus,
        addr = POWER_ADDR,
        shunt_res = SHUNT_OHMS,
        max_expected_current = MAX_EXPECTED_CURRENT_A,
    })
end

local function try_open(label, addr, fn)
    local ok, err = pcall(fn)
    if not ok then
        print(string.format("[sensors] %s at 0x%02X did not answer: %s",
            label, addr, (tostring(err):gsub("\n.*", ""))))
        return false
    end
    return true
end

local function run()
    bus = i2c.new(I2C_PORT, I2C_SDA, I2C_SCL, I2C_FREQ_HZ)

    local want_light = ACTION == "all" or ACTION == "light"
    local want_power = ACTION == "all" or ACTION == "power"
    local have_light = want_light and try_open("light sensor", LIGHT_ADDR, open_light)
    local have_power = want_power and try_open("power monitor", POWER_ADDR, open_power)

    if not have_light and not have_power then
        error("no requested sensor answered on I2C port " .. I2C_PORT
            .. "; that is the bus rather than one chip")
    end

    for i = 1, ctx.samples do
        if ctx.samples > 1 then
            print(string.format("[sensors] --- sample %d/%d ---", i, ctx.samples))
        end
        if have_light then
            report_light()
        end
        if have_power then
            report_power()
        end
        if i < ctx.samples and ctx.interval_ms > 0 then
            delay.delay_ms(ctx.interval_ms)
        end
    end

    -- The battery percentage is deliberately absent: this board has no
    -- state-of-charge curve, and guessing one from voltage would read as fact.
    if have_power then
        print("[sensors] no state-of-charge available; report the voltage, do not"
            .. " convert it to a percentage")
    end
end

-- 8. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[sensors] ERROR: " .. (tostring(err):gsub("\n.*", "")))
    error(err)
end
