-- Environmental sensor demo: open a DUT and exercise all exported methods.
-- Optional args:
--   type="bme690"|"dht"
--   device="<board device name>"      -- for bme690
--   pin=<gpio>                         -- for dht
--   sensor_type="dht11"|"dht22"|...   -- for dht
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

local function run()
    local backend_type
    local opts

    local function build_opts(kind)
        if kind == "dht" then
            return {
                type = "dht",
                pin = DHT_PIN,
                sensor_type = DHT_SENSOR_TYPE,
            }
        end
        return {
            type = "bme690",
            device = DUT,
        }
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

    if REQUESTED_BACKEND_TYPE ~= nil then
        backend_type = REQUESTED_BACKEND_TYPE
        sensor, opts = open_sensor(backend_type)
    else
        local ok, opened_sensor, open_opts = pcall(open_sensor, "bme690")
        if ok then
            backend_type = "bme690"
            sensor = opened_sensor
            opts = open_opts
        else
            print("[environmental_sensor] bme690 open failed, falling back to dht")
            print("[environmental_sensor] fallback reason: " .. tostring(opened_sensor))
            backend_type = "dht"
            sensor, opts = open_sensor(backend_type)
        end
    end

    print("[environmental_sensor] opened " .. sensor:name())

    print("[environmental_sensor] calling sensor:read()")
    local sample = sensor:read()
    print(string.format("temperature: %.2f C", sample.temperature))
    print(string.format("humidity: %.2f %%", sample.humidity))

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
    else
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
    end

    print("[environmental_sensor] calling sensor:close()")
    sensor:close()
    sensor = nil
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
