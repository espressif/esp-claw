local si12t_touch = require("lib_si12t_touch")
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

local I2C_ADDR = int_arg("addr", nil)
local FREQ_HZ = int_arg("freq_hz", 100000)
local SAMPLE_COUNT = int_arg("samples", 200)
local INTERVAL_MS = int_arg("interval_ms", 50)
local THRESHOLD = int_arg("threshold", nil)

local touch
local bus

local function cleanup()
    if touch then
        pcall(function()
            touch:close()
        end)
        touch = nil
    end
end

local function mask_to_bits(mask, n)
    local s = ""
    for i = 0, n - 1 do
        if (mask >> i) & 0x1 == 1 then
            s = s .. "1"
        else
            s = s .. "0"
        end
    end
    return s
end

local function run()
    local port = int_arg("port", 0)
    local sda = int_arg("sda", 39)
    local scl = int_arg("scl", 40)
    bus = a.bus or i2c.new(port, sda, scl, FREQ_HZ)

    local opts = {
        bus = bus,
        freq_hz = FREQ_HZ,
        channels = a.channels,
    }
    if I2C_ADDR then
        opts.addr = I2C_ADDR
    end
    if THRESHOLD then
        opts.threshold = THRESHOLD
    end

    touch = si12t_touch.new(opts)
    local channels = si12t_touch.channels()
    print(string.format(
        "[si12t_touch] opened addr=0x%02X threshold=%d mask=0x%03X",
        touch:address(), touch:threshold(), touch:channel_mask()
    ))

    local last_mask = -1
    local change_count = 0

    do
        local mask = touch:read()
        last_mask = mask
        change_count = change_count + 1
        print(string.format(
            "[si12t_touch] #%d TS1~TS%d: %s (0x%03X) [init]",
            change_count, channels, mask_to_bits(mask, channels), mask
        ))
    end

    for _ = 1, SAMPLE_COUNT do
        delay.delay_ms(INTERVAL_MS)
        local mask = touch:read()
        if mask ~= last_mask then
            change_count = change_count + 1
            print(string.format(
                "[si12t_touch] #%d TS1~TS%d: %s (0x%03X)",
                change_count, channels, mask_to_bits(mask, channels), mask
            ))
            last_mask = mask
        end
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
