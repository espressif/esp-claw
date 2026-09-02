local sx1262 = require("sx1262")

-- Change this to the legal frequency supported by the installed HPD16A.
local frequency_hz = assert(args and args.frequency_hz, "frequency_hz is required")
local radio

local function run()
    radio = sx1262.new({
        spi_host = 2,
        cs_gpio = 14,
        reset_gpio = 42,
        dio1_gpio = 45,
        busy_gpio = 38,
        frequency_hz = frequency_hz,
        bandwidth_khz = 125,
        spreading_factor = 9,
        coding_rate = 6,
        sync_word = 0xAB,
    })

    assert(radio:start_receive())
    while true do
        local packet, err = radio:receive(1000)
        if packet then
            print(string.format("RX %d bytes RSSI %.1f SNR %.1f: %s",
                packet.length, packet.rssi, packet.snr, packet.data))
            local ok, send_err = radio:send("ACK:" .. packet.data, 5000)
            assert(ok, send_err)
            assert(radio:start_receive())
        elseif err ~= "timeout" then
            print("RX error: " .. tostring(err))
        end
    end
end

local ok, err = xpcall(run, debug.traceback)
if radio then
    pcall(function() radio:close() end)
end
if not ok then
    error(err)
end

