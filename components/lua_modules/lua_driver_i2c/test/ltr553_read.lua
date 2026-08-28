-- Read ambient light (and optionally proximity) from an LTR-553ALS.
-- Defaults target M5Stack CoreS3 / StackChan: internal I2C bus, address 0x23.

local ltr553 = require("lib_ltr553")
local delay = require("delay")
local i2c = require("i2c")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local PORT = int_arg("port", 0)
local SDA = int_arg("sda", 12)
local SCL = int_arg("scl", 11)
local FREQ_HZ = int_arg("freq_hz", 400000)
local I2C_ADDR = int_arg("addr", 0x23)
local GAIN = int_arg("gain", 1)
local INTEGRATION_MS = int_arg("integration_time_ms", 100)
local RATE_MS = int_arg("measurement_rate_ms", 100)
local SAMPLE_COUNT = int_arg("samples", 20)
local INTERVAL_MS = int_arg("interval_ms", 200)
local ENABLE_PS = a.enable_ps == true

local sensor
local bus
local owns_bus = false

local function cleanup()
    if sensor then
        pcall(function() sensor:close() end)
        sensor = nil
    end
    if owns_bus and bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

local function run()
    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(PORT, SDA, SCL, FREQ_HZ)
        owns_bus = true
    end

    sensor = ltr553.new({
        bus = bus,
        addr = I2C_ADDR,
        gain = GAIN,
        integration_time_ms = INTEGRATION_MS,
        measurement_rate_ms = RATE_MS,
        enable_ps = ENABLE_PS,
    })

    local part_number, revision = sensor:part_id()
    print(string.format(
        "[ltr553] opened addr=0x%02X part=0x%X rev=0x%X manufacturer=0x%02X",
        sensor:address(), part_number, revision, sensor:manufacturer_id()))
    print(string.format(
        "[ltr553] gain=%dX integration=%dms rate=%dms proximity=%s",
        sensor:gain(), sensor:integration_time_ms(), sensor:measurement_rate_ms(),
        ENABLE_PS and "on" or "off"))

    -- The first sample only becomes meaningful after one full integration cycle.
    delay.delay_ms(INTEGRATION_MS)

    for i = 1, SAMPLE_COUNT do
        local s = sensor:read()
        local line = string.format(
            "[ltr553] #%d %.2f lux ch0=%d ch1=%d new=%s",
            i, s.lux, s.ch0, s.ch1, tostring(s.als_new_data))
        if s.data_invalid then
            line = line .. " INVALID"
        end
        if s.proximity then
            line = line .. string.format(" prox=%d%s", s.proximity,
                s.proximity_saturated and " SAT" or "")
        end
        print(line)
        if i < SAMPLE_COUNT then
            delay.delay_ms(INTERVAL_MS)
        end
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
