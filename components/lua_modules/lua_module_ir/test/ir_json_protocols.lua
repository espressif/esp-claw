-- Every air-conditioner protocol, builtin table or bundled JSON, honors its own capabilities; no hardware is accessed.
local ir = require("ir")
local ir_remote = require("ir_remote")
local delay = require("delay")

-- Each codeset scans the bundle several times; yielding lets the idle task feed the watchdog.
local YIELD_MS = 10

local function rejects(fn, text)
    local ok, err = pcall(fn)
    assert(not ok, "expected rejection")
    if text then assert(string.find(tostring(err), text, 1, true), tostring(err)) end
end

local BUILTIN_AC_COUNT, IREXT_COUNT = 8, 58
local codesets = ir.protocol.list({category = "air_conditioner"})
assert(#codesets == BUILTIN_AC_COUNT + IREXT_COUNT)
local gree = ir.protocol.list({category = "air_conditioner", brand = "Gree"})
assert(#gree >= 6)
for _, info in ipairs(gree) do assert(info.category == "air_conditioner" and info.brand == "Gree") end
rejects(function() ir.protocol.list({kind = "ac"}) end, "unknown field")

local irext, brands = 0, {}
for _, info in ipairs(codesets) do
    assert(info.id and info.brand and info.maturity and #info.fields > 0, tostring(info.id))
    if info.id:find("%.irext_") then
        irext = irext + 1
        brands[info.brand] = true
        assert(info.source:match("^irext:") and not info.decode, info.id)
    end

    local p = ir.protocol.get(info.id)
    assert(p:info().id == info.id)
    local caps = p:capabilities()
    local remote = ir_remote.new({protocol = info.id})
    local state = remote:state()
    for field, cap in pairs(caps.state) do
        assert(state[field] == cap.default, info.id .. " default " .. field)
    end
    assert(p:check(state), info.id)
    assert(#remote:set({power = true}) > 0, info.id)
    assert(#remote:set({power = false}) > 0, info.id)
    assert(remote:state().power == false)
    remote:set({power = true})

    local mode_caps = caps.state.mode and p:capabilities({mode = state.mode}).state or caps.state
    local temperature = mode_caps.temperature
    if temperature then
        assert(#remote:set({temperature = temperature.max}) > 0, info.id)
        rejects(function() remote:set({temperature = temperature.max + temperature.step}) end, "unsupported value")
        assert(remote:state().temperature == temperature.max)
    end
    for _, fan in ipairs(mode_caps.fan and mode_caps.fan.values or {}) do
        assert(#remote:set({fan = fan}) > 0, info.id .. " fan " .. fan)
    end
    for _, key in ipairs(p:capabilities({mode = remote:state().mode}).keys) do
        assert(remote:keys()[key] == "function" and #remote:press(key) > 0, info.id .. " key " .. key)
        -- The power key flips the cached power; pressing it again restores power for the next keys.
        if key == "power" and remote:state().power ~= nil then
            assert(remote:state().power == false and #remote:press(key) > 0 and remote:state().power == true, info.id)
        end
    end
    delay.delay_ms(YIELD_MS)
end
assert(irext == IREXT_COUNT)
local brand_count = 0
for _ in pairs(brands) do brand_count = brand_count + 1 end
assert(brand_count == 13)

-- Capability corrections: a one-frame codeset becomes keys only; duplicate swing encodings are merged.
local pcaps = ir.protocol.get("panasonic.irext_25073"):capabilities()
assert(pcaps.state.mode == nil and pcaps.state.power and #pcaps.keys >= 1)
assert(ir.protocol.get("panasonic.irext_25074"):capabilities().state.swing_v == nil)

-- Builtin AC tables describe themselves: Xiaomi decodes within its mode limits, dry runs at low fan only.
local xiaomi = ir.protocol.get("xiaomi.96")
local xiaomi_info = xiaomi:info()
assert(xiaomi_info.category == "air_conditioner" and xiaomi_info.brand == "Xiaomi" and xiaomi_info.model == "96-bit" and xiaomi_info.decode)
local dry = {power = true, mode = "dry", temperature = 24, fan = "low", swing_v = "off", sleep = false}
local values, info = xiaomi:decode(xiaomi:encode(dry))
assert(values.temperature == 24 and values.fan == "low" and values.protocol == nil and info.checked)
local off, off_info = xiaomi:decode(xiaomi:encode({power = false}))
assert(off.power == false and off.mode == nil and off_info.message == "off")
local irext = ir.protocol.get("gree.irext_10727")
assert(select(2, irext:decode(xiaomi:encode({power = false}))) == "not_decodable")
dry.fan = "high"
rejects(function() xiaomi:encode(dry) end, "unsupported value")
rejects(function() xiaomi:encode({protocol = "xiaomi.96", power = false}) end, "unknown field 'protocol'")
assert(not xiaomi:check({mode = "dry", fan = "high"}) and xiaomi:check({mode = "cool", fan = "high"}))
assert(irext:encode({power = false}):info().carrier_hz == 38000)
rejects(function() ir.protocol.get("Bad id") end, "unknown protocol")
print("[ir_json_protocols] passed; bundled JSON protocols load and encode within their limits")
