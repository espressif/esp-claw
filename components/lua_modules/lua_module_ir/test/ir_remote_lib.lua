-- ir_remote library: keys, learning, AC state and snapshots; no hardware is accessed.
local ir = require("ir")
local ir_remote = require("ir_remote")

local function rejects(fn, text)
    local ok, err = pcall(fn)
    assert(not ok, "expected rejection")
    if text then assert(string.find(tostring(err), text, 1, true), tostring(err)) end
end

local function same(a, b)
    a, b = a:to_table(), b:to_table()
    assert(a.carrier_hz == b.carrier_hz and #a.pulses_us == #b.pulses_us, "frame length")
    for i, pulse in ipairs(a.pulses_us) do assert(pulse == b.pulses_us[i], "pulse " .. i) end
end

local nec_protocol = ir.protocol.get("nec")
local function nec(command)
    return nec_protocol:encode({address = 0x10, command = command})
end

-- Mapped keys on a builtin protocol.
local keys = {power = {command = 0x20}, volume_up = {command = 0x21}}
local tv = ir_remote.new({protocol = "nec", fixed = {address = 0x10}, keys = keys})
keys.power.command = 0xFF
same(nec(0x20), tv:press("power"))
assert(#tv:press("volume_up", {hold_ms = 600}) == 68 + 5 * 4)
local listed = tv:keys()
assert(listed.power == "mapped" and listed.volume_up == "mapped")
assert(tv:info().id == "nec" and tv:info().category == nil)
assert(tv:decode(nec(0x21)).key == "volume_up")
assert(select(2, tv:decode(nec(0x30))) == "no_match")
rejects(function() tv:press("mute") end, "unknown key")
rejects(function() tv:press("power", {hold_ms = -1}) end, "hold_ms")
rejects(function() tv:set({power = true}) end, "unsupported value")

-- Learning overrides mapped keys; frames are stored as received, never decoded, and only matched with match_raw.
local raw = ir.frame({carrier_hz = 40000, pulses_us = {900, 450, 100, 200, 300}})
local mute = ir.protocol.get("samsung32"):encode({address = 7, command = 9})
assert(tv:learn("netflix", raw) == nil)
tv:learn("mute", mute)
same(raw, tv:press("netflix"))
same(mute, tv:press("mute"))
same(mute, tv:press("mute", {hold_ms = 600}))
assert(tv:keys().netflix == "learned" and tv:keys().mute == "learned")
assert(select(2, tv:decode(mute)) == "no_match")
assert(select(2, tv:decode(raw)) == "no_match")
tv:learn("power", raw)
same(raw, tv:press("power"))
assert(tv:forget("power") and not tv:forget("power"))
same(nec(0x20), tv:press("power"))
rejects(function() tv:learn("", raw) end, "invalid key name")
rejects(function() tv:learn("x", {pulses_us = {1}}) end, "ir.frame")

local snapshot = tv:to_table()
assert(snapshot.version == 1 and snapshot.protocol == "nec" and snapshot.fixed.address == 0x10)
assert(snapshot.keys.power.command == 0x20 and #snapshot.learned.mute.pulses_us == #mute and snapshot.learned.mute.protocol == nil)
local restored = ir_remote.new(snapshot)
snapshot.learned.netflix.pulses_us[1] = 1
same(raw, restored:press("netflix"))
same(tv:press("mute"), restored:press("mute"))
same(nec(0x21), restored:press("volume_up"))

local matcher = ir_remote.new({match_raw = true, learned = {netflix = raw:to_table()}})
assert(matcher:decode(ir.frame({carrier_hz = 38000, pulses_us = {950, 420, 110, 190, 310}})).key == "netflix")
assert(next(ir_remote.new({}):keys()) == nil and ir_remote.new():info().id == nil)
rejects(function() ir_remote.new({}):press("power") end, "unknown key")
rejects(function() ir_remote.new({profile = "gree"}) end, "unknown field 'profile'")
rejects(function() ir_remote.new({fixed = {address = 1}}) end, "need a protocol")
rejects(function() ir_remote.new({protocol = "missing"}) end, "unknown protocol")
rejects(function() ir_remote.new({version = 2}) end, "unsupported snapshot version")
rejects(function() ir_remote.new({protocol = "nec", keys = {power = {command = 1, speed = 2}}}) end, "unknown field")
rejects(function() ir_remote.new({protocol = "gree", fixed = {address = 1}}) end, "unknown field 'address'")
rejects(function() ir_remote.new({learned = {mute = {protocol = "nec", address = 1, command = 2}}}) end, "unknown field")

-- The toggle bit flips on every new press.
local rc5 = ir_remote.new({protocol = "rc5", fixed = {address = 1}, keys = {ok = {command = 2}}})
local rc5_protocol = ir.protocol.get("rc5")
local t1, t2 = rc5_protocol:decode(rc5:press("ok")).toggle, rc5_protocol:decode(rc5:press("ok")).toggle
assert(t1 ~= t2)
assert(rc5:decode(rc5_protocol:encode({address = 1, command = 2, toggle = true})).key == "ok")

-- AC state: defaults, merging, limits, automatic fitting and decode.
local ac = ir_remote.new({protocol = "gree"})
local state = ac:state()
assert(state.power == true and state.mode == "cool" and state.temperature == 26 and state.fan == "auto")
local gree = ir.protocol.get("gree")
same(gree:encode({power = true, mode = "cool", temperature = 24, fan = "auto"}), ac:set({temperature = 24}))
assert(ac:state().temperature == 24)
rejects(function() ac:set({temperature = 31}) end, "unsupported value")
rejects(function() ac:set({swing_v = "auto"}) end, "unsupported value")
assert(ac:state().temperature == 24)
ac:set({power = false})
assert(ac:state().power == false)
local event = ac:decode(gree:encode({power = true, mode = "heat", temperature = 28, fan = "high"}))
assert(event.state.mode == "heat" and ac:state().temperature == 28 and ac:state().fan == "high")
local caps = ac:capabilities()
assert(caps.state.temperature.type == "number" and caps.state.temperature.min == 16 and caps.state.temperature.max == 30)
assert(caps.state.temperature.step == 1 and caps.state.temperature.default == 26 and #caps.state.mode.values == 5)
assert(next(caps.keys) == nil and next(ac:keys()) == nil)
rejects(function() ac:press("power") end, "unknown key")

local irext = ir_remote.new({protocol = "gree.irext_10727"})
local dry = irext:capabilities({mode = "dry"})
assert(dry.state.temperature.min == 16 and dry.state.temperature.max == 30 and #dry.state.fan.values == 1)
irext:set({mode = "dry"})
state = irext:state()
assert(state.temperature == 26 and state.fan == "low")
rejects(function() irext:set({fan = "high"}) end, "unsupported value")
irext:set({mode = "cool", temperature = 27.0, fan = "high"})
assert(irext:state().temperature == 27)

local ac_snapshot = irext:to_table()
assert(ac_snapshot.protocol == "gree.irext_10727" and ac_snapshot.state.fan == "high")
local irext2 = ir_remote.new(ac_snapshot)
same(irext:set({temperature = 25}), irext2:set({temperature = 25}))
rejects(function() ir_remote.new({protocol = "gree.irext_10727", state = {temperature = 99}}) end, "state")
rejects(function() ir_remote.new({state = {power = true}}) end, "state needs")

-- Function keys on an IRext codeset: power + state keys, picked automatically on set.
local hisense = ir_remote.new({protocol = "hisense.irext_11677"})
local hkeys = hisense:capabilities().keys
assert(hkeys[1] == "power" and hisense:keys().power == "function")
assert(#hisense:press("power") > 0)
hisense:set({mode = "auto"})
assert(#hisense:set({temperature = 27}) > 0)
assert(select(2, hisense:decode(hisense:press("power"))) == "not_decodable")

for _ = 1, 10 do
    collectgarbage("collect")
    same(nec(0x20), tv:press("power"))
end
print("[ir_remote_lib] passed; keys, learning, AC state and snapshots")
