-- Optional args: type="bme690"|"dht", port=0, address=0x76, pin, sensor_type="dht22".
-- BME690 uses an initialized board I2C bus; DHT requires an explicitly chosen free pin.
local environmental_sensor = require("environmental_sensor")
local a = type(args) == "table" and args or {}
local sensor, bus

local ok, err = xpcall(function()
    local kind = a.type or "bme690"
    if kind == "bme690" then
        bus = require("i2c").borrow(a.port or 0)
        sensor = environmental_sensor.new({type = kind, bus = bus, address = a.address or 0x76})
    else
        assert(kind == "dht", "type must be bme690 or dht")
        assert(a.pin ~= nil, "choose a free DHT GPIO with args.pin")
        sensor = environmental_sensor.new({type = kind, pin = a.pin, sensor_type = a.sensor_type or "dht22"})
    end
    local sample = sensor:read()
    print(sensor:name(), sample.temperature, sample.humidity)
    if kind == "bme690" then
        print(sample.pressure, sample.gas_resistance, sample.status)
        print(sensor:chip_id(), sensor:variant_id())
    end
end, debug.traceback)

-- Keep cleanup ordered even when opening or reading fails.
local closed, close_err = true, nil
if sensor then closed, close_err = pcall(function() sensor:close() end) end
if closed and bus then closed, close_err = pcall(function() bus:close() end) end
assert(ok, err)
assert(closed, close_err)
