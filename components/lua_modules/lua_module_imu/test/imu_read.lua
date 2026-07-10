-- IMU demo: open board device, print accel/gyro/temperature samples.
-- Optional args: device, samples, interval_ms.
local imu = require("imu")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local DEVICE_NAME = type(a.device) == "string" and a.device or "imu_sensor"
local SAMPLE_COUNT = int_arg("samples", 20)
local INTERVAL_MS = int_arg("interval_ms", 200)

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
    print("[imu] opening device " .. DEVICE_NAME)
    sensor = imu.new(DEVICE_NAME)
    print("[imu] opened " .. sensor:name())

    for i = 1, SAMPLE_COUNT do
        local sample = sensor:read()
        local temp = sensor:read_temperature()
        print(string.format(
            "[imu] #%d acc=(%d,%d,%d) gyr=(%d,%d,%d) temp_raw=%d status=0x%02X",
            i,
            sample.accel.x, sample.accel.y, sample.accel.z,
            sample.gyro.x, sample.gyro.y, sample.gyro.z,
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
