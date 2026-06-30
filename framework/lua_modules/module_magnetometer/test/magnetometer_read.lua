-- Magnetometer demo: open board device and print magnetic field samples.
-- Optional args: device, samples, interval_ms, calibrate.
local magnetometer = require("magnetometer")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local DEVICE_NAME = type(a.device) == "string" and a.device or "magnetometer_sensor"
local SAMPLE_COUNT = int_arg("samples", 20)
local INTERVAL_MS = int_arg("interval_ms", 200)
local RUN_CALIBRATION = a.calibrate == true

local sensor

local function cleanup()
    if sensor then
        pcall(function()
            sensor:close()
        end)
        sensor = nil
    end
end

local function run()
    print("[magnetometer] opening device " .. DEVICE_NAME)
    sensor = magnetometer.new(DEVICE_NAME)
    print("[magnetometer] opened " .. sensor:name())
    if RUN_CALIBRATION then
        print("[magnetometer] collecting calibration samples")
        sensor:calibration_reset()
        for _ = 1, SAMPLE_COUNT do
            sensor:calibration_add_sample()
            delay.delay_ms(INTERVAL_MS)
        end
        local cal = sensor:calibration_finish()
        print(string.format(
            "[magnetometer] calibration done hard_iron=(%.3f,%.3f,%.3f)",
            cal.hard_iron[1], cal.hard_iron[2], cal.hard_iron[3]
        ))
    end

    for i = 1, SAMPLE_COUNT do
        local sample = sensor:read()
        local temp = sample.temperature
        print(string.format(
            "[magnetometer] #%d mag=(%.3f,%.3f,%.3f) raw=(%.3f,%.3f,%.3f) temp=%.3fC status=0x%02X",
            i,
            sample.magnetic.x, sample.magnetic.y, sample.magnetic.z,
            sample.raw_magnetic.x, sample.raw_magnetic.y, sample.raw_magnetic.z,
            temp,
            sample.status
        ))
        delay.delay_ms(INTERVAL_MS)
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
