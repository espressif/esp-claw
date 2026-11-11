-- Built-in protocol encode/decode regression; no hardware is accessed.
local ir = require("ir")

local function rejects(fn, text)
    local ok, err = pcall(fn)
    assert(not ok, "expected rejection")
    if text then assert(string.find(tostring(err), text, 1, true), tostring(err)) end
end

local function read_bits(pulses, first, count, mark, zero, one, lsb)
    local value = 0
    for bit = 0, count - 1 do
        assert(pulses[first + 2 * bit] == mark)
        local space = pulses[first + 2 * bit + 1]
        assert(space == zero or space == one)
        if space == one then value = value | (1 << (lsb and bit or count - bit - 1)) end
    end
    return value
end

local protocols = {}
for _, p in ipairs(ir.protocol.list()) do protocols[p.id] = p end
assert(protocols.nec.decode and protocols.nec.checked and protocols.nec.carrier_hz == 38000)
assert(protocols.sony12.decode and not protocols.sony12.checked and protocols.sony12.carrier_hz == 40000)
assert(table.concat(protocols.rc5.fields, ",") == "address,command,toggle")
assert(table.concat(protocols.gree.fields, ",") == "power,mode,temperature,fan,swing_v")
assert(protocols.gree.category == "air_conditioner" and protocols.gree.decode and protocols.nec.category == nil)

local cases = {
    {"nec", {address = 0x10, command = 0xEF}, count = 68, carrier = 38000, copies = 1},
    {"nec.extended", {address = 0x1234, command = 0x56}, count = 68, carrier = 38000, copies = 1},
    {"samsung32", {address = 0x12, command = 0x56}, count = 68, carrier = 38000, copies = 1},
    {"sony12", {address = 0x1F, command = 0x7F}, count = 78, carrier = 40000, copies = 3},
    {"sony15", {address = 0xFF, command = 0x7F}, count = 96, carrier = 40000, copies = 3},
    {"sony20", {address = 0x1FFF, command = 0x7F}, count = 126, carrier = 40000, copies = 3},
    {"rc5", {address = 0x1F, command = 0x3F, toggle = true}, count = 28, carrier = 36000, copies = 1},
    {"gree", {power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "auto"}, count = 279, carrier = 38000, copies = 1},
    {
        "haier.a6", {power = true, mode = "heat", temperature = 30, fan = "high", swing_v = "position_3", sleep = true, key = "mode"},
        count = 229, carrier = 38000, copies = 1,
    },
    {"daikin.280", {power = true, mode = "cool", temperature = 25, fan = "medium", swing_v = "auto", swing_h = "off"}, count = 583, carrier = 38000, copies = 1},
    {"midea.coolix", {power = true, mode = "cool", temperature = 22, fan = "high"}, count = 299, carrier = 38000, copies = 1},
    {
        "hisense.kelon168", {power = true, mode = "cool", temperature = 27, fan = "low", swing_v = "position_1", key = "fan"},
        count = 343, carrier = 38000, copies = 1,
    },
    {
        "aux.electra", {power = true, mode = "heat", temperature = 28, fan = "medium", swing_v = "auto", sleep = false, key = "temp_up"},
        count = 211, carrier = 38000, copies = 1,
    },
    {"tcl.112", {power = true, mode = "dry", temperature = 23, fan = "min", swing_v = "off"}, count = 227, carrier = 38000, copies = 1},
    {"xiaomi.96", {power = true, mode = "heat", temperature = 30, fan = "high", swing_v = "off", sleep = true}, count = 99, carrier = 38000, copies = 1},
}
for _, case in ipairs(cases) do
    local id, values = case[1], case[2]
    local p = ir.protocol.get(id)
    local frame = p:encode(values)
    assert(#frame == case.count and frame:info().carrier_hz == case.carrier, id)
    local decoded, info = p:decode(frame)
    assert(decoded and decoded.protocol == nil, id)
    for key, value in pairs(values) do assert(decoded[key] == value, id .. " " .. key) end
    assert(info.message == "frame" and info.copies == case.copies and info.next == case.count + 1)
    assert(info.checked == protocols[id].checked)
    assert(#p:encode(decoded) == case.count, id .. " re-encode")
end

-- JVC in 38 kHz carrier cycles: 320/160 header, 20-cycle marks, 20/60-cycle spaces, 55 ms period.
local jvc = ir.protocol.get("jvc")
for command = 0, 255 do
    local frame = jvc:encode({address = 255 - command, command = command})
    local pulses = frame:to_table().pulses_us
    assert(#pulses == 36 and pulses[1] == 8421 and pulses[2] == 4210 and frame:info().duration_us == 55000)
    assert(read_bits(pulses, 3, 8, 526, 526, 1578, true) == 255 - command)
    assert(read_bits(pulses, 19, 8, 526, 526, 1578, true) == command)
    assert(pulses[35] == 526)
end

-- Builtin AC tables carry limits: the Haier 4-bit temperature field reaches 31 degrees, but a checked 31 degree frame is unsupported.
local HAIER_HEADER_PULSES, HAIER_MARK, HAIER_ZERO, HAIER_ONE = 4, 510, 590, 1700
local function haier_byte_first(index) return HAIER_HEADER_PULSES + 1 + 16 * index end
local haier = ir.protocol.get("haier.a6")
local haier_values = {power = true, mode = "cool", temperature = 30, fan = "auto", swing_v = "position_5", sleep = false, key = "temp_up"}
local hot = haier:encode(haier_values):to_table()
local function write_byte(index, value)
    for bit = 0, 7 do
        hot.pulses_us[haier_byte_first(index) + 2 * bit + 1] = ((value >> (7 - bit)) & 1) == 1 and HAIER_ONE or HAIER_ZERO
    end
end
local function read_byte(index) return read_bits(hot.pulses_us, haier_byte_first(index), 8, HAIER_MARK, HAIER_ZERO, HAIER_ONE, false) end
write_byte(13, (read_byte(13) + 0x10) & 0xFF)
write_byte(1, read_byte(1) | 0x10)
assert(select(2, haier:decode(ir.frame(hot))) == "unsupported_value")
haier_values.temperature = 31
rejects(function() haier:encode(haier_values) end, "unsupported value")

-- Toggle defaults to 0 and is part of the code.
local rc5 = ir.protocol.get("rc5")
assert(rc5:decode(rc5:encode({address = 1, command = 2})).toggle == false)

-- Flip one bit of the inverted NEC command byte.
local nec = ir.protocol.get("nec")
local pulses = nec:encode({address = 0x10, command = 0xEF}):to_table()
local bit_space = 4 + 2 * 24
pulses.pulses_us[bit_space] = pulses.pulses_us[bit_space] == 560 and 1690 or 560
local decoded, reason = nec:decode(ir.frame(pulses))
assert(decoded == nil and reason == "checksum")
decoded, reason = nec:decode(ir.frame({pulses_us = {100, 200, 300}}))
assert(decoded == nil and reason == "no_match")
assert(select(2, ir.protocol.get("sony12"):decode(nec:encode({address = 1, command = 2}))) == "no_match")

-- A second frame after a gap is reached through info.next.
local samsung = ir.protocol.get("samsung32")
local first = nec:encode({address = 1, command = 2}):to_table().pulses_us
local second = samsung:encode({address = 3, command = 4}):to_table().pulses_us
for _, pulse in ipairs(second) do first[#first + 1] = pulse end
local joined = ir.frame({carrier_hz = 38000, pulses_us = first})
local _, info = nec:decode(joined)
assert(info.next == 69 and samsung:decode(joined, {offset = info.next}).command == 4)

-- Holding a key appends hold messages: NEC repeat codes every 108 ms.
assert(#nec:encode({address = 1, command = 2}, {hold_ms = 600}) == 68 + 5 * 4)
local sony = ir.protocol.get("sony12")
assert(#sony:encode({address = 1, command = 2}, {hold_ms = 200}) > #sony:encode({address = 1, command = 2}))

local gree = ir.protocol.get("gree")
assert(ir.encode == nil and ir.decode == nil)
rejects(function() nec:encode({address = 1, command = 2}, {hold = 1}) end, "unknown field")
rejects(function() nec:encode({address = 1, command = 2}, {hold_ms = 1.5}) end, "hold_ms")
rejects(function() nec:encode({protocol = "nec", address = 1, command = 2}) end, "unknown field 'protocol'")
rejects(function() ir.protocol.get("missing") end, "unknown protocol")
rejects(function() nec:encode({address = 1}) end, "missing or invalid field")
rejects(function() nec:encode({address = 256, command = 1}) end, "unsupported value")
rejects(function() nec:encode({address = 1, command = 1, speed = 1}) end, "unknown field")
rejects(function() gree:encode({power = true, mode = "cool", temperature = 24.25, fan = "auto", swing_v = "off"}) end, "at most one decimal")
rejects(function() gree:encode({power = true, mode = "cold", temperature = 24, fan = "auto", swing_v = "off"}) end, "invalid mode")
rejects(function() gree:encode({power = true, mode = "cool", temperature = 24, fan = "auto"}) end, "missing or invalid field")
rejects(function() nec:decode(joined, {offset = 0}) end, "offset")
rejects(function() nec:decode(joined, {protocol = "nec"}) end, "unknown field")
rejects(function() ir.frame({pulses_us = {1, 2}, start_level = 1}) end, "unknown field")
rejects(function() ir.frame({pulses_us = {[1] = 1, [3] = 2}}) end, "dense array")
rejects(function() ir.frame({pulses_us = {0}}) end, "pulse[1]")
print("[ir_codec] passed; software encode/decode only")
