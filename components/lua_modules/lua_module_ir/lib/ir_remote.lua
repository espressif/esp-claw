-- Stateful IR remote on top of the ir module: learned keys, mapped keys, AC state and snapshots.
local ir = require("ir")

local M = {}
local Remote = {}
Remote.__index = Remote

local VERSION = 1
local KEY_NAME_MAX = 32
local LEARNED_MAX = 64
local LEARNED_PULSES_MAX = 16384
local RAW_TOLERANCE = 0.25
local RAW_TOLERANCE_US = 100
local STEP_EPSILON = 1e-3
local CONFIG_FIELDS = {version = true, protocol = true, fixed = true, keys = true, state = true, learned = true, match_raw = true}
-- Fields whose change selects a function key, in priority order; temperature and swing_v are resolved below.
local KEY_FIELDS = {"power", "mode", "temperature", "fan", "swing_v", "swing_h", "turbo", "quiet", "econo", "light", "sleep", "clean", "filter", "beep"}

local function fail(fmt, ...) error("ir_remote: " .. string.format(fmt, ...), 0) end

local function copy(t)
    local out = {}
    for k, v in pairs(t or {}) do out[k] = v end
    return out
end

local function merge(a, b)
    local out = copy(a)
    for k, v in pairs(b or {}) do out[k] = v end
    return out
end

local function set_of(list)
    local out = {}
    for _, v in ipairs(list or {}) do out[v] = true end
    return out
end

local function check_table(value, name)
    if value ~= nil and type(value) ~= "table" then fail("%s must be a table", name) end
end

local function check_key_name(name)
    if type(name) ~= "string" or #name < 1 or #name > KEY_NAME_MAX then fail("invalid key name") end
end

-- True when every field of subset except toggle has the same value in values.
local function contains(values, subset)
    for k, v in pairs(subset) do
        if k ~= "toggle" and values[k] ~= v then return false end
    end
    return true
end

local function raw_match(learned, frame)
    if #learned ~= #frame then return false end
    local a, b = learned:to_table().pulses_us, frame:to_table().pulses_us
    for i, pulse in ipairs(a) do
        if math.abs(pulse - b[i]) > math.max(RAW_TOLERANCE_US, pulse * RAW_TOLERANCE) then return false end
    end
    return true
end

-- Closest value a field limit allows: the first listed value, or the range clamp rounded down to a step.
local function nearest(cap, value)
    if cap.values then return cap.values[1] end
    if not cap.min or type(value) ~= "number" then return value end
    local v = math.max(cap.min, math.min(cap.max, value))
    return cap.min + math.floor((v - cap.min) / cap.step + STEP_EPSILON) * cap.step
end

function Remote:_function_keys(mode)
    return self._protocol and self._protocol:capabilities({mode = mode}).keys or {}
end

-- Learned keys are raw frames, replayed exactly as received.
function Remote:_store(name, frame)
    check_key_name(name)
    local old = self._learned[name]
    local count = old and 0 or 1
    for _ in pairs(self._learned) do count = count + 1 end
    if count > LEARNED_MAX then fail("at most %d learned keys", LEARNED_MAX) end
    local pulses = self._learned_pulses - (old and #old or 0) + #frame
    if pulses > LEARNED_PULSES_MAX then fail("learned frames exceed %d pulses", LEARNED_PULSES_MAX) end
    self._learned[name] = frame
    self._learned_pulses = pulses
end

function Remote:_init_protocol(config)
    local p = ir.protocol.get(config.protocol)
    local fields = set_of(p:info().fields)
    self._protocol, self._fields = p, fields
    for name in pairs(config.fixed or {}) do
        if not fields[name] then fail("unknown field '%s'", tostring(name)) end
    end
    if not p:check(self._fixed) then fail("unsupported value in fixed") end
    for name, values in pairs(config.keys or {}) do
        check_key_name(name)
        check_table(values, "key")
        self._keys[name] = copy(values)
        p:encode(merge(self._fixed, values))
    end
    local caps = p:capabilities().state
    for field, cap in pairs(caps) do self._state[field] = cap.default end
    for field, value in pairs(config.state or {}) do
        if not caps[field] then fail("invalid state field '%s'", tostring(field)) end
        self._state[field] = value
    end
    if not p:check(self._state) then fail("unsupported state value") end
end

function M.new(config)
    config = config or {}
    check_table(config, "config")
    if config.version ~= nil and config.version ~= VERSION then fail("unsupported snapshot version") end
    for name in pairs(config) do
        if not CONFIG_FIELDS[name] then fail("unknown field '%s'", tostring(name)) end
    end
    for _, name in ipairs({"fixed", "keys", "state", "learned"}) do check_table(config[name], name) end
    local self = setmetatable({_fixed = copy(config.fixed), _keys = {}, _state = {}, _fields = {}, _learned = {}, _learned_pulses = 0, _toggle = false}, Remote)
    self._match_raw = config.match_raw == true
    if config.protocol ~= nil then
        self:_init_protocol(config)
    elseif config.fixed ~= nil or config.keys ~= nil then
        fail("fixed and keys need a protocol")
    elseif config.state ~= nil then
        fail("state needs a protocol")
    end
    for name, value in pairs(config.learned or {}) do
        check_table(value, "learned entry")
        self:_store(name, ir.frame(value))
    end
    return self
end

-- Sets the toggle bit of protocols that carry one, flipping it for the next press.
function Remote:_toggled(values, has_toggle)
    if has_toggle then
        values.toggle = self._toggle
        self._toggle = not self._toggle
    end
    return values
end

function Remote:press(key, options)
    local learned = self._learned[key]
    if learned then return learned end
    local p = self._protocol
    local values = self._keys[key]
    if values then return p:encode(self:_toggled(merge(self._fixed, values), self._fields.toggle), options) end
    if p and set_of(self:_function_keys(self._state.mode))[key] then
        -- The power key flips the cached power so protocols with an off frame or a toggle stay in sync.
        if key == "power" and self._state.power ~= nil then return self:_apply({power = not self._state.power, key = key}, options) end
        values = merge(self._fixed, self._state)
        values.key = key
        return p:encode(values, options)
    end
    fail("unknown key '%s'", tostring(key))
end

-- Function key for a state change: the first changed field whose key the mode allows, else the first allowed key,
-- skipping power while power is unchanged so toggle protocols are not switched.
local function pick_key(old, new, keys)
    local allowed = set_of(keys)
    for _, field in ipairs(KEY_FIELDS) do
        local key = field
        if field == "temperature" then
            key = (new.temperature or 0) > (old.temperature or 0) and "temp_up" or "temp_down"
        elseif field == "swing_v" then
            key = tostring(new.swing_v):match("^position_") and allowed.swing_v_fix and "swing_v_fix" or "swing_v"
        end
        if new[field] ~= old[field] and allowed[key] then return key end
    end
    for _, key in ipairs(keys) do
        if key ~= "power" or new.power ~= old.power then return key end
    end
    return "power"
end

function Remote:_apply(changes, options)
    local p = self._protocol
    if not p then fail("unsupported value: no protocol") end
    local caps = p:capabilities().state
    local new = copy(self._state)
    for field, value in pairs(changes) do
        if field ~= "key" then
            if not caps[field] then fail("unsupported value for '%s'", tostring(field)) end
            new[field] = value
        end
    end
    -- Fields missing from the mode caps are not sent in that mode; their cached values are kept for later modes.
    local mode_caps = p:capabilities({mode = new.mode}).state
    for field in pairs(changes) do
        if field ~= "key" and not mode_caps[field] then fail("unsupported value for '%s' in mode '%s'", tostring(field), tostring(new.mode)) end
    end
    for field, cap in pairs(mode_caps) do
        if changes[field] == nil and new[field] ~= nil and not p:check({mode = new.mode, [field] = new[field]}) then new[field] = nearest(cap, new[field]) end
    end
    local values = merge(self._fixed, new)
    if self._fields.key then
        values.key = changes.key or pick_key(self._state, new, self:_function_keys(new.mode))
    elseif changes.key ~= nil then
        fail("unsupported value for 'key'")
    end
    local frame = p:encode(values, options)
    self._state = new
    return frame
end

function Remote:set(changes)
    if type(changes) ~= "table" then fail("changes must be a table") end
    return self:_apply(changes)
end

function Remote:state() return copy(self._state) end

function Remote:capabilities(context)
    if not self._protocol then return {state = {}, keys = {}} end
    return self._protocol:capabilities(context)
end

function Remote:keys()
    local out = {}
    for _, key in ipairs(self:_function_keys(self._state.mode)) do out[key] = "function" end
    for key in pairs(self._keys) do out[key] = "mapped" end
    for key in pairs(self._learned) do out[key] = "learned" end
    return out
end

function Remote:learn(key, frame)
    if type(frame) ~= "userdata" then fail("frame must be an ir.frame") end
    -- Frames are immutable, so the received frame is kept as is.
    self:_store(key, frame)
end

function Remote:forget(key)
    local learned = self._learned[key]
    if not learned then return false end
    self._learned_pulses = self._learned_pulses - #learned
    self._learned[key] = nil
    return true
end

function Remote:decode(frame)
    if self._match_raw then
        for name, learned in pairs(self._learned) do
            if raw_match(learned, frame) then return {key = name} end
        end
    end
    if not self._protocol then return nil, "no_match" end
    local values, reason = self._protocol:decode(frame)
    if not values then return nil, reason end
    for name, key_values in pairs(self._keys) do
        if contains(values, merge(self._fixed, key_values)) then return {key = name} end
    end
    local state = {}
    for field in pairs(self._protocol:capabilities().state) do state[field] = values[field] end
    if next(state) == nil then return nil, "no_match" end
    self._state = merge(self._state, state)
    return {state = state}
end

function Remote:info()
    return self._protocol and self._protocol:info() or {}
end

function Remote:to_table()
    local snapshot = {version = VERSION, learned = {}, match_raw = self._match_raw or nil}
    if self._protocol then
        snapshot.protocol = self._protocol:info().id
        snapshot.fixed = next(self._fixed) and copy(self._fixed) or nil
        snapshot.keys = {}
        for name, values in pairs(self._keys) do snapshot.keys[name] = copy(values) end
        snapshot.state = next(self._state) and copy(self._state) or nil
    end
    for name, learned in pairs(self._learned) do snapshot.learned[name] = learned:to_table() end
    return snapshot
end

return M
