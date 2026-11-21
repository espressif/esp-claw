-- ir.protocol.register(): Lua tables in the JSON protocol format register encode only protocols in this Lua state; no hardware is accessed.
local ir = require("ir")
local ir_remote = require("ir_remote")

local none = ir.protocol.none

local function rejects(fn, text)
    local ok, err = pcall(fn)
    assert(not ok, "expected rejection")
    if text then assert(string.find(tostring(err), text, 1, true), tostring(err)) end
end

local function same(a, b)
    local x, y = a:to_table(), b:to_table()
    if x.carrier_hz ~= y.carrier_hz or #x.pulses_us ~= #y.pulses_us then return false end
    for i, pulse in ipairs(x.pulses_us) do
        if pulse ~= y.pulses_us[i] then return false end
    end
    return true
end

-- The builtin Gree table, transcribed field by field.
local function gree_description(id)
    local temperatures = {}
    for i = 0, 14 do temperatures[#temperatures + 1] = i | i << 4 end
    return {
        id = id,
        category = "air_conditioner",
        brand = "Gree",
        maturity = "experimental",
        protocol = {
            carrier_hz = 38000,
            messages = {{
                layout = {
                    symbols = {{590, 600}, {590, 1700}},
                    steps = {"M9010", "S4520", "B35", "M590", "S20020", "B32", "M590", "S40000", "M9010", "S4520", "B35", "M590", "S20020", "B32", "M590"},
                },
                length = 18,
                widths = {{4, 3}, {13, 3}},
                template = "000020500210200000000020700200000000",
                fields = {
                    {field = "power", segments = {{0, 3, 1}, {9, 3, 1}}, raw = {0, 3}},
                    {field = "mode", segments = {{0, 0, 3}, {9, 0, 3}}, raw = {0, 9, 18, 36, 27}},
                    {field = "fan", segments = {{0, 4, 2}, {9, 4, 2}, {16, 4, 3}}, raw = {0, none, 21, 63, 95}},
                    {field = "temperature", segments = {{1, 0, 4}, {10, 0, 4}}, raw = temperatures, base = 160, step = 10},
                    {field = "swing_v", segments = {{0, 6, 1}, {9, 6, 1}, {5, 0, 4}}, raw = {0, 7}},
                },
                checks = {
                    {op = "sum", nibble = true, units = {1, 3, 5, 7, 10, 12, 14}, target = 16, bias = 10},
                    {op = "sum", nibble = true, units = {19, 21, 23, 25, 28, 30, 32}, target = 34, bias = 10},
                },
            }},
        },
        defaults = {power = true, mode = "cool", temperature = 26, fan = "auto", swing_v = "off"},
        limits = {
            mode = {"auto", "cool", "dry", "heat", "fan"}, temperature = {min = 16, max = 30, step = 1}, fan = {"auto", "low", "medium", "high"},
            swing_v = {"off", "auto"},
        },
    }
end

-- 32-bit power field: raw codes above 2^24 must reach C exactly.
local function wide_description(id, on)
    return {
        id = id,
        protocol = {
            carrier_hz = 38000,
            messages = {{
                implied = {},
                layout = {symbols = {{560, 560}, {560, 1690}}, steps = {"M9000", "S4500", "B32", "M560"}},
                length = 4,
                fields = {{field = "power", segments = {{0, 0, 8}, {1, 0, 8}, {2, 0, 8}, {3, 0, 8}}, raw = {0, on}}},
            }},
        },
    }
end

local function list_ids(filter)
    local ids = {}
    for _, p in ipairs(ir.protocol.list(filter)) do ids[#ids + 1] = p.id end
    return ids
end

local function index_of(list, value)
    for i, v in ipairs(list) do
        if v == value then return i end
    end
end

assert(type(none) == "userdata")

local builtin = ir.protocol.get("gree")
local base_count = #list_ids()
-- Builtin tables are listed first and are the only decodable protocols.
local builtin_count = 0
for _, p in ipairs(ir.protocol.list()) do
    if not p.decode then break end
    builtin_count = builtin_count + 1
end
local json_count = base_count - builtin_count
local gree = ir.protocol.register(gree_description("test.gree"))
local count = 0
for _, power in ipairs({true, false}) do
    for _, mode in ipairs({"auto", "cool", "dry", "heat", "fan"}) do
        for _, fan in ipairs({"auto", "low", "medium", "high"}) do
            for temperature = 16, 30, 7 do
                local values = {power = power, mode = mode, temperature = temperature, fan = fan, swing_v = count % 2 == 0 and "off" or "auto"}
                assert(same(gree:encode(values), builtin:encode(values)), mode .. " " .. fan .. " " .. temperature)
                count = count + 1
            end
        end
    end
end
assert(count == 120)

local info = gree:info()
assert(info.id == "test.gree" and info.decode == false and info.checked == false and info.carrier_hz == 38000)
assert(info.category == "air_conditioner" and info.brand == "Gree" and info.maturity == "experimental")
assert(table.concat(info.fields, ",") == table.concat(builtin:info().fields, ","))
local values, reason = gree:decode(builtin:encode({power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "off"}))
assert(values == nil and reason == "not_decodable")
local caps = gree:capabilities()
assert(caps.state.temperature.min == 16 and caps.state.temperature.max == 30 and caps.state.temperature.default == 26)
assert(gree:check({temperature = 30}) and not gree:check({temperature = 31}))
rejects(function() gree:encode({power = true, mode = "cool", temperature = 31, fan = "auto", swing_v = "off"}) end, "unsupported value")
rejects(function() gree:encode({power = true, mode = "cool", temperature = 24, fan = "min", swing_v = "off"}) end, "unsupported value")

-- Registration: get() returns the same object, a second registration of the id is rejected.
assert(rawequal(ir.protocol.get("test.gree"), gree))
assert(getmetatable(gree) == false)
rejects(function() ir.protocol.register(gree_description("test.gree")) end, "already registered")

local wide = ir.protocol.register(wide_description("test.wide1", 16777217))
assert(not same(wide:encode({power = true}), ir.protocol.register(wide_description("test.wide0", 16777216)):encode({power = true})))

-- A registered id hides the JSON codeset with the same id in this Lua state.
local json_gree = ir.protocol.get("gree.irext_2969")
local shadow = ir.protocol.register(gree_description("gree.irext_2969"))
assert(json_gree:info().model and shadow:info().model == nil and rawequal(ir.protocol.get("gree.irext_2969"), shadow))

-- list(): builtin tables, registered protocols in registration order, then JSON codesets without hidden ids.
local ids = list_ids()
assert(#ids == base_count + 3, #ids)
local first = builtin_count + 1
assert(ids[first] == "test.gree" and ids[first + 1] == "test.wide1" and ids[first + 2] == "test.wide0" and ids[first + 3] == "gree.irext_2969")
assert(index_of(ids, "gree.irext_10727") > first + 3 and #ids - (first + 3) == json_count - 1)
local gree_ids = list_ids({brand = "Gree"})
assert(gree_ids[1] == "gree" and gree_ids[2] == "test.gree" and gree_ids[3] == "gree.irext_2969" and index_of(gree_ids, "test.wide1") == nil)

rejects(function() ir.protocol.register("nec") end, "table expected")
rejects(function() ir.protocol.register(gree_description("gree")) end, "builtin protocol id")
rejects(function() ir.protocol.register(gree_description("Bad id")) end, "missing or invalid field")
rejects(function() ir.protocol.register({id = "test.ref", protocol = "nec"}) end, "missing or invalid field")
rejects(function() ir.protocol.register({id = "test.fn", protocol = function() end}) end, "unsupported function value")
rejects(function() ir.protocol.register({id = "test.mixed", "nec"}) end, "table keys")
local bad = wide_description("test.bad", 1)
bad.protocol.messages[1].layout.steps = {"M9000", "B9"}
bad.protocol.messages[1].length = 1
bad.protocol.messages[1].fields = {}
rejects(function() ir.protocol.register(bad) end, "too large")
local deep = {}
for _ = 1, 20 do deep = {deep} end
rejects(function() ir.protocol.register({id = "test.deep", protocol = deep}) end, "nested too deep")
-- Failed registrations leave nothing behind.
rejects(function() ir.protocol.get("test.bad") end, "unknown protocol")
assert(#list_ids() == base_count + 3)

-- Four symbols carry two bits each: a JSON copy of the xiaomi.96 off frame sends the builtin pulses.
local function quad_description(id, steps)
    return {
        id = id,
        protocol = {
            carrier_hz = 38000,
            messages = {{
                implied = {power = false},
                layout = {symbols = {{580, 400}, {580, 900}, {580, 2230}, {580, 1500}}, steps = steps},
                length = 12, msb_first = true, template = "B62300108501003000000031",
            }},
        },
    }
end
local quad = ir.protocol.register(quad_description("test.quad", {"M1000", "S600", "B96", "M580"}))
assert(same(quad:encode({power = false}), ir.protocol.get("xiaomi.96"):encode({power = false})))
rejects(function() ir.protocol.register(quad_description("test.quad_odd", {"M1000", "S600", "B95", "M580", "B1"})) end, "missing or invalid field")

-- A null mode limit marks a field the mode does not send: it is neither offered nor checked in that mode.
local fan_only = gree_description("test.gree_fan")
fan_only.mode_limits = {fan = {temperature = none}}
local unused = ir.protocol.register(fan_only)
assert(unused:capabilities({mode = "fan"}).state.temperature == nil and unused:capabilities({mode = "cool"}).state.temperature.max == 30)
assert(unused:check({mode = "fan", temperature = 99}) and not unused:check({mode = "cool", temperature = 99}))
-- A mode limit narrows a field: dry is fixed at 24 degrees, and ir_remote fits the state when entering dry.
local fixed_dry = gree_description("test.gree_dry")
fixed_dry.mode_limits = {dry = {temperature = {min = 24, max = 24, step = 1}}}
ir.protocol.register(fixed_dry)
local dry_remote = ir_remote.new({protocol = "test.gree_dry"})
local dry_caps = dry_remote:capabilities({mode = "dry"}).state.temperature
assert(dry_caps.min == 24 and dry_caps.max == 24)
dry_remote:set({mode = "dry"})
assert(dry_remote:state().temperature == 24)
rejects(function() dry_remote:set({temperature = 26}) end, "unsupported value")
local null_limit = gree_description("test.gree_null")
null_limit.limits.temperature = none
rejects(function() ir.protocol.register(null_limit) end, "missing or invalid field")

-- ir_remote takes the id like any other protocol; snapshots restore in the same Lua state.
local remote = ir_remote.new({protocol = "test.gree"})
local frame = remote:set({temperature = 24})
assert(same(frame, builtin:encode({power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "off"})))
local snapshot = remote:to_table()
assert(snapshot.protocol == "test.gree" and snapshot.state.temperature == 24)
local restored = ir_remote.new(snapshot)
assert(restored:state().temperature == 24 and same(restored:set({fan = "high"}), remote:set({fan = "high"})))
rejects(function() ir_remote.new({protocol = gree}) end, "string expected")

print("ir_protocol_register ok")
