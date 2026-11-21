local ir = require("ir")

local options = type(args) == "table" and args or {}
local function option(name, default)
    local value = options[name]
    return value == nil and default or value
end

local dev

local function open_device()
    return ir.device({
        name = "box3_ir_smoke",
        tx_gpio = option("tx_gpio", 39),
        rx_gpio = option("rx_gpio", 38),
        ctrl_gpio = option("ctrl_gpio", 44),
        ctrl_active_level = option("ctrl_active_level", 0),
    })
end

local function cleanup()
    if dev then
        pcall(dev.stop_receive, dev)
        pcall(dev.close, dev)
        dev = nil
    end
end

local function check_receive_result(frame, err)
    if frame then
        assert(type(frame) == "userdata" and #frame > 0)
    else
        assert(err == "timeout" or err == "empty_frame" or err == "frame_too_long", tostring(err))
    end
end

local function run()
    dev = open_device()
    assert(ir.new == nil)

    local info = dev:info()
    assert(info.tx_gpio == option("tx_gpio", 39))
    assert(info.rx_gpio == option("rx_gpio", 38))
    assert(info.ctrl_gpio == option("ctrl_gpio", 44))
    assert(not pcall(dev.receive, dev, 30))
    assert(not pcall(dev.start_receive, dev, {timeout_ms = 30}))
    assert(dev:start_receive())
    assert(not pcall(dev.read, dev, 30))
    assert(dev:stop_receive())

    local first, first_err = dev:receive({timeout_ms = option("receive_timeout_ms", 30)})
    check_receive_result(first, first_err)
    local second, second_err = dev:receive({timeout_ms = option("receive_timeout_ms", 30)})
    check_receive_result(second, second_err)

    -- Force TX timeout, then verify that the channel can be recreated.
    local timeout_frame = ir.frame({
        carrier_hz = 1000,
        pulses_us = {100000, 100000},
    })
    local timeout_ok, timeout_err = pcall(dev.send, dev, timeout_frame, {timeout_ms = 1})
    assert(not timeout_ok and string.find(tostring(timeout_err), "transmit wait failed", 1, true), tostring(timeout_err))

    -- A 1 kHz synthetic waveform exercises TX without representing a household IR command.
    local smoke_frame = ir.frame({
        carrier_hz = 1000,
        pulses_us = {1000, 1000, 1000, 1000},
    })
    local repeat_ok, repeat_err = pcall(dev.send, dev, smoke_frame, {repeat_count = 2})
    assert(not repeat_ok and string.find(tostring(repeat_err), "unknown field 'repeat_count'", 1, true), tostring(repeat_err))
    local gap_ok, gap_err = pcall(dev.send, dev, smoke_frame, {repeat_gap_us = 1000})
    assert(not gap_ok and string.find(tostring(gap_err), "unknown field 'repeat_gap_us'", 1, true), tostring(gap_err))
    -- Serialized waveforms require explicit restoration before transmission.
    local saved = smoke_frame:to_table()
    local table_ok, table_err = pcall(dev.send, dev, saved)
    assert(not table_ok and string.find(tostring(table_err), "ir.frame expected", 1, true), tostring(table_err))
    assert(dev:send(ir.frame(saved), {timeout_ms = 1000}))
    assert(dev:send(smoke_frame, {timeout_ms = 1000}))

    if option("loopback", false) then
        local loopback_frame = ir.frame({
            carrier_hz = 38000,
                pulses_us = {700, 700, 700, 2100, 700, 700},
        })
        assert(dev:start_receive())
        assert(dev:send(loopback_frame, {timeout_ms = 1000}))
        local frame, err = dev:read({timeout_ms = option("loopback_timeout_ms", 200)})
        assert(frame, "loopback did not receive a frame: " .. tostring(err))
        assert(type(frame) == "userdata" and #frame > 0)
    end

    dev:close()
    dev:close()
    dev = nil

    dev = open_device()
    assert(dev:info().name == "box3_ir_smoke")
    dev:close()
    dev = nil
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then error(err) end
print("[ir_device_smoke] passed; no appliance command was transmitted")
