-- Optical loopback on one board. Synthetic waveforms by default; real protocol frames only with args.protocols = true.
local ir = require("ir")
local delay = require("delay")
local options = type(args) == "table" and args or {}
local dev

local function option(name, default)
    local value = options[name]
    return value == nil and default or value
end

local function check_waveform(frame, expected, label)
    local pulses = frame:to_table().pulses_us
    print("[ir_loopback] " .. label .. " pulses=" .. table.concat(pulses, ","))
    -- Idle termination may add one trailing space.
    local count = #pulses % 2 == 0 and #pulses - 1 or #pulses
    assert(count == #expected, label .. ": pulse count mismatch")
    for i, duration in ipairs(expected) do
        local tolerance = math.max(180, duration * 0.25)
        assert(math.abs(pulses[i] - duration) <= tolerance, label .. ": pulse " .. i .. " expected " .. duration .. ", got " .. pulses[i])
    end
end

local function same_frame(frame, saved)
    local raw = frame:to_table()
    assert(raw.carrier_hz == saved.carrier_hz and #raw.pulses_us == #saved.pulses_us)
    for i, duration in ipairs(saved.pulses_us) do assert(raw.pulses_us[i] == duration, "previous RX frame changed") end
end

local function exchange(frame, label)
    assert(dev:start_receive())
    assert(dev:send(frame, {timeout_ms = 2000}))
    local received, err = dev:read({timeout_ms = 1000})
    assert(received, label .. ": " .. tostring(err))
    return received
end

-- Each decodable built-in protocol must survive encode, send, receive and decode.
local PROTOCOL_CODES = {
    {"nec", {address = 0x10, command = 0x20}},
    {"nec.extended", {address = 0x1234, command = 0x56}},
    {"samsung32", {address = 0x12, command = 0x34}},
    {"sony12", {address = 0x01, command = 0x15}},
    {"sony15", {address = 0x5A, command = 0x15}},
    {"sony20", {address = 0x1A5, command = 0x15}},
    {"rc5", {address = 0x05, command = 0x21}},
    {"jvc", {address = 0x10, command = 0x20}},
    {"gree", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "off"}},
    {"haier.a6", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "position_5", sleep = false, key = "power"}},
    {"daikin.280", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "off", swing_h = "auto"}},
    {"midea.coolix", {power = true, mode = "cool", temperature = 24, fan = "auto"}},
    {"hisense.kelon168", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "position_1", key = "mode"}},
    {"aux.electra", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "off", sleep = false, key = "power"}},
    {"tcl.112", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "off"}},
    {"xiaomi.96", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "auto", sleep = false}},
}

local function run_protocols()
    for _, case in ipairs(PROTOCOL_CODES) do
        local id, values = case[1], case[2]
        local p = ir.protocol.get(id)
        local decoded, reason = p:decode(exchange(p:encode(values), id))
        assert(decoded, id .. ": " .. tostring(reason))
        for key, value in pairs(values) do assert(decoded[key] == value, id .. ": " .. key) end
        print("[ir_loopback] decoded " .. id)
        delay.delay_ms(100)
    end
end

local function run()
    dev = ir.device({
        name = "box3_ir_loopback",
        tx_gpio = option("tx_gpio", 39),
        rx_gpio = option("rx_gpio", 38),
        ctrl_gpio = option("ctrl_gpio", 44),
        ctrl_active_level = option("ctrl_active_level", 0),
    })
    local first_pulses = {700, 700, 700, 2100, 700, 1400, 700}
    local second_pulses = {900, 1800, 600, 900, 900, 2700, 600, 1200, 900}
    local first_tx = ir.frame({carrier_hz = 38000, pulses_us = first_pulses})
    -- Warm up the powered receiver and lazy TX channel before measuring.
    assert(dev:send(first_tx))
    delay.delay_ms(100)
    local first_rx = exchange(first_tx, "first")
    check_waveform(first_rx, first_pulses, "first")
    local saved = first_rx:to_table()
    check_waveform(exchange(ir.frame({carrier_hz = 38000, pulses_us = second_pulses}), "second"), second_pulses, "second")
    same_frame(first_rx, saved)
    check_waveform(exchange(ir.frame(saved), "learned replay"), first_pulses, "learned replay")
    same_frame(first_rx, saved)

    if option("long_gap", false) then
        local pulses = {700, 1400, 700, 40000, 700, 2100, 700}
        check_waveform(exchange(ir.frame({carrier_hz = 38000, pulses_us = pulses}), "40ms gap"), pulses, "40ms gap")
    end
    if option("protocols", false) then run_protocols() end
    dev:close()
    dev = nil
end

local ok, err = xpcall(run, debug.traceback)
if dev then
    pcall(dev.stop_receive, dev)
    pcall(dev.close, dev)
end
if not ok then error(err) end
print("[ir_loopback] passed; appliance compatibility not tested")
