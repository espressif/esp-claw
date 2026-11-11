-- Read voltage / current / power from an INA226.
-- Defaults target M5Stack StackChan: CoreS3 internal I2C bus, 0x41, 10 mOhm shunt.

local ina226 = require("lib_ina226")
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
local function num_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return v
    end
    return default
end

local PORT = int_arg("port", 0)
local SDA = int_arg("sda", 12)
local SCL = int_arg("scl", 11)
local FREQ_HZ = int_arg("freq_hz", 400000)
local I2C_ADDR = int_arg("addr", 0x41)
local SHUNT_RES = num_arg("shunt_res", 0.01)
local MAX_CURRENT = num_arg("max_expected_current", 8.19)
local SAMPLE_COUNT = int_arg("samples", 10)
local INTERVAL_MS = int_arg("interval_ms", 500)
local CHECK_ID = a.check_id ~= false and a.check_die_id ~= false

local monitor
local bus
local owns_bus = false

local function cleanup()
    if monitor then
        pcall(function() monitor:close() end)
        monitor = nil
    end
    if owns_bus and bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

-- Probe the two identity registers directly, so a failed constructor can say
-- whether anything answers at this address at all.
local function report_identity()
    local dev = bus:device(I2C_ADDR, 0)
    for _, entry in ipairs({{0xFE, "manufacturer"}, {0xFF, "die"}}) do
        local ok, raw = pcall(dev.read, dev, 2, entry[1])
        if ok then
            local value = (string.byte(raw, 1) << 8) | string.byte(raw, 2)
            print(string.format("[ina226] raw %s id (0x%02X) = 0x%04X",
                entry[2], entry[1], value))
        else
            print(string.format("[ina226] raw %s id (0x%02X) read failed: %s",
                entry[2], entry[1], tostring(raw)))
        end
    end
    print("[ina226] expected: manufacturer 0x5449, die 0x2260")
    pcall(function() dev:close() end)
end

local function run()
    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(PORT, SDA, SCL, FREQ_HZ)
        owns_bus = true
    end

    local ok, result = pcall(ina226.new, {
        bus = bus,
        addr = I2C_ADDR,
        shunt_res = SHUNT_RES,
        max_expected_current = MAX_CURRENT,
        check_id = CHECK_ID,
    })
    if not ok then
        print(string.format("[ina226] open failed at 0x%02X: %s", I2C_ADDR, tostring(result)))
        report_identity()
        print("[ina226] hint: scan the bus first, then retry with " ..
              "--args-json '{\"check_id\":false}' to read anyway")
        error(result, 0)
    end
    monitor = result

    print(string.format(
        "[ina226] opened addr=0x%02X manufacturer=0x%04X die_id=0x%04X%s",
        monitor:address(), monitor:manufacturer_id(), monitor:die_id(),
        monitor:die_id_is_standard() and "" or " (non-standard, not a fault)"))
    print(string.format(
        "[ina226] shunt=%.4f ohm max=%.2f A", SHUNT_RES, MAX_CURRENT))
    print(string.format(
        "[ina226] current_lsb=%.9f A/count calibration=%d",
        monitor:current_lsb(), monitor:calibration()))

    for i = 1, SAMPLE_COUNT do
        local s = monitor:read()
        print(string.format(
            "[ina226] #%d bus=%.3f V shunt=%.6f V current=%+.3f A power=%.3f W (%s)",
            i, s.bus_voltage, s.shunt_voltage, s.current, s.power,
            s.current < 0 and "charging" or "discharging"))
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
