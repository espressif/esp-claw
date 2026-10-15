local twai = require("twai")

local bus = twai.new({
    tx_gpio = 6,
    rx_gpio = 7,
    bitrate = 500000,
    mode = "no_ack",
})

local ok, err = xpcall(function()
    assert(bus:send(0x123, {0, 0, 0, 0, 0, 0, 0, 1}, {
        self_reception = true,
        timeout_ms = 100,
    }))
    local frame, rx_err = bus:receive(1000)
    assert(frame, rx_err)
    assert(frame.id == 0x123)
    assert(frame.dlc == 8)
    assert(frame.data[8] == 1)
end, debug.traceback)

bus:close()
assert(ok, err)
print("twai_self_test PASS")
