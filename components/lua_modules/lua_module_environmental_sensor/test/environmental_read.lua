-- Environmental sensor demo: open a DUT and print display-safe values.
-- Optional args:
--   type="shtc3"|"bme690"|"dht"
--   device="<board device name>"      -- for SHTC3/BME690
--   pin=<gpio>                         -- for DHT
--   sensor_type="dht11"|"dht22"|...   -- for DHT
local environmental_sensor = require("environmental_sensor")

local a = type(args) == "table" and args or {}

local REQUESTED_BACKEND_TYPE = type(a.type) == "string" and a.type or nil
local DUT = type(a.device) == "string" and a.device or "environmental_sensor"
local DHT_PIN = type(a.pin) == "number" and math.floor(a.pin) or 4
local DHT_SENSOR_TYPE = type(a.sensor_type) == "string" and a.sensor_type or "dht22"

local sensor

local function cleanup()
    if sensor then
        pcall(function()
            sensor:close()
        end)
        sensor = nil
    end
end

local function build_opts(kind)
    if kind == "dht" then
        return {
            type = "dht",
            pin = DHT_PIN,
            sensor_type = DHT_SENSOR_TYPE,
        }
    end
    return {
        type = kind,
        device = DUT,
    }
end

local function print_na(reason)
    print("[environmental_sensor] sample_ok=false error=" .. tostring(reason))
    print("temperature: N/A C")
    print("humidity: N/A %")
end

local function open_sensor(kind)
    local open_opts = build_opts(kind)
    if kind == "dht" then
        print(string.format(
            "[environmental_sensor] opening dut=%s type=%s pin=%d sensor_type=%s",
            DUT, kind, DHT_PIN, DHT_SENSOR_TYPE
        ))
    else
        print(string.format(
            "[environmental_sensor] opening dut=%s type=%s",
            DUT, kind
        ))
    end
    return environmental_sensor.new(open_opts), open_opts
end

local function open_first_available()
    local kinds = REQUESTED_BACKEND_TYPE ~= nil and { REQUESTED_BACKEND_TYPE } or { "shtc3", "bme690", "dht" }
    local last_err

    for _, kind in ipairs(kinds) do
        local ok, opened_sensor, open_opts = pcall(open_sensor, kind)
        if ok then
            return kind, opened_sensor, open_opts
        end
        last_err = opened_sensor
        print(string.format("[environmental_sensor] %s open failed: %s", kind, tostring(opened_sensor)))
    end

    return nil, nil, nil, last_err
end

local function run()
    local backend_type, opts, open_err
    backend_type, sensor, opts, open_err = open_first_available()
    if not sensor then
        print_na(open_err or "open failed")
        return
    end

    print("[environmental_sensor] opened " .. sensor:name())
    print("[environmental_sensor] calling sensor:read_safe()")
    local sample = sensor:read_safe()
    if not sample.ok then
        print_na(sample.error or "read failed")
        return
    end

    print("[environmental_sensor] sample_ok=true")
    print(string.format("temperature: %s C", sample.temperature_display))
    print(string.format("humidity: %s %%", sample.humidity_display))

    if sample.raw_temperature ~= nil then
        print(string.format("raw_temperature=0x%04X", sample.raw_temperature))
    end
    if sample.raw_humidity ~= nil then
        print(string.format("raw_humidity=0x%04X", sample.raw_humidity))
    end
    if sample.temperature_crc ~= nil then
        print(string.format("temperature_crc=0x%02X", sample.temperature_crc))
    end
    if sample.humidity_crc ~= nil then
        print(string.format("humidity_crc=0x%02X", sample.humidity_crc))
    end

    print("[environmental_sensor] calling sensor:read_temperature()")
    local temperature = sensor:read_temperature()
    print(string.format("temperature_only: %.2f C", temperature))

    print("[environmental_sensor] calling sensor:read_humidity()")
    local humidity = sensor:read_humidity()
    print(string.format("humidity_only: %.2f %%", humidity))

    if backend_type == "dht" then
        print("[environmental_sensor] calling sensor:read_raw()")
        local temp_raw, humidity_raw = sensor:read_raw()
        print(string.format("raw_temperature=%d raw_humidity=%d", temp_raw, humidity_raw))
    elseif backend_type == "bme690" then
        print("[environmental_sensor] calling sensor:read_pressure()")
        local pressure = sensor:read_pressure()
        print(string.format("pressure_only: %.2f Pa", pressure))

        print("[environmental_sensor] calling sensor:read_gas()")
        local gas = sensor:read_gas()
        print(string.format("gas_only: %.2f ohm", gas))

        print("[environmental_sensor] calling sensor:chip_id()")
        print(string.format("chip_id: 0x%02X", sensor:chip_id()))

        print("[environmental_sensor] calling sensor:variant_id()")
        print(string.format("variant_id: %d", sensor:variant_id()))
    elseif backend_type == "shtc3" then
        print("[environmental_sensor] calling sensor:product_id()")
        print(string.format("product_id: 0x%04X", sensor:product_id()))
    end

    if sample.pressure ~= nil then
        print(string.format("pressure: %.2f Pa", sample.pressure))
    end
    if sample.gas_resistance ~= nil then
        print(string.format("gas resistance: %.2f ohm", sample.gas_resistance))
    end
    if sample.status ~= nil then
        print(string.format("status: 0x%02X", sample.status))
    end
    if sample.gas_index ~= nil then
        print(string.format("gas_index: %d", sample.gas_index))
    end
    if sample.meas_index ~= nil then
        print(string.format("meas_index: %d", sample.meas_index))
    end

    print("[environmental_sensor] calling sensor:close()")
    sensor:close()
    sensor = nil
    opts = nil
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print_na(err)
end
