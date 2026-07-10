local i2c = require("i2c")

local M = {}

local DEFAULT_FREQ_HZ = 400000

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- Byte helpers
-- ---------------------------------------------------------------------------

local function u16le(data)
    local lo = string.byte(data, 1) or 0
    local hi = string.byte(data, 2) or 0
    return lo | (hi << 8)
end

local function i16le(data)
    local v = u16le(data)
    if v >= 0x8000 then
        return v - 0x10000
    end
    return v
end

local function u16be(data)
    local hi = string.byte(data, 1) or 0
    local lo = string.byte(data, 2) or 0
    return (hi << 8) | lo
end

-- ---------------------------------------------------------------------------
-- Chip profiles
--
-- Each profile defines the register map and read helpers for one fuel-gauge
-- IC. To add a new chip, create a new entry with the same fields and add
-- the corresponding Kconfig option.
-- ---------------------------------------------------------------------------

local CHIP_PROFILES = {
    bq27220 = {
        default_addr = 0x55,
        read_voltage_mv = function(dev)
            return u16le(dev:read(2, 0x08))
        end,
        read_current_ma = function(dev)
            return i16le(dev:read(2, 0x0C))
        end,
        read_soc = function(dev)
            return u16le(dev:read(2, 0x2C))
        end,
    },

    max17048 = {
        default_addr = 0x36,
        read_voltage_mv = function(dev)
            return math.floor(u16be(dev:read(2, 0x02)) * 78.125 / 1000)
        end,
        read_current_ma = nil, -- MAX17048 has no current register
        read_soc = function(dev)
            return (u16be(dev:read(2, 0x04)) >> 8)
        end,
    },

    -- stc3115 = { ... },  -- add future chips here
}

-- ---------------------------------------------------------------------------
-- Resolve which chip profile to use.
--   1. Explicit opts.chip overrides everything.
--   2. Otherwise fall back to a compile-time default via _G.FUEL_GAUGE_CHIP
--      (set by the C registration shim, if any).
--   3. Last resort: "bq27220".
-- ---------------------------------------------------------------------------

local function resolve_chip(opts)
    local name = opts.chip
        or (type(_G.FUEL_GAUGE_CHIP) == "string" and _G.FUEL_GAUGE_CHIP)
        or "bq27220"
    name = string.lower(name)
    local profile = CHIP_PROFILES[name]
    if not profile then
        error(string.format(
            "fuel_gauge.new: unsupported chip '%s' (available: %s)",
            name, table.concat(M.supported_chips(), ", ")))
    end
    return name, profile
end

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

local function new_device_from_opts(opts, default_addr)
    local bus
    local owns_bus = false

    if opts.bus ~= nil then
        bus = opts.bus
    else
        bus = i2c.new(
            assert(opts.port, "fuel_gauge.new: missing 'port'"),
            assert(opts.sda, "fuel_gauge.new: missing 'sda'"),
            assert(opts.scl, "fuel_gauge.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = opts.close_bus == true
    end

    local dev = bus:device(opts.addr or default_addr, 0)
    return bus, dev, owns_bus
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local chip_name, profile = resolve_chip(opts)
    local bus, dev, owns_bus = new_device_from_opts(opts, profile.default_addr)
    return setmetatable({
        _bus = bus,
        _dev = dev,
        _owns_bus = owns_bus,
        _addr = opts.addr or profile.default_addr,
        _chip = chip_name,
        _profile = profile,
    }, mt)
end

function M.supported_chips()
    local names = {}
    for k in pairs(CHIP_PROFILES) do
        names[#names + 1] = k
    end
    table.sort(names)
    return names
end

-- ---------------------------------------------------------------------------
-- Instance methods
-- ---------------------------------------------------------------------------

function mt:chip()
    return self._chip
end

function mt:address()
    return self._addr
end

function mt:read_voltage_mv()
    return self._profile.read_voltage_mv(self._dev)
end

function mt:read_current_ma()
    local fn = self._profile.read_current_ma
    if not fn then
        error(string.format("fuel_gauge: chip '%s' does not support current reading", self._chip))
    end
    return fn(self._dev)
end

function mt:read_soc()
    return self._profile.read_soc(self._dev)
end

function mt:read()
    local result = {
        chip = self._chip,
        voltage_mv = self:read_voltage_mv(),
        soc = self:read_soc(),
    }
    if self._profile.read_current_ma then
        result.current_ma = self._profile.read_current_ma(self._dev)
    end
    return result
end

function mt:close()
    if self._dev then
        self._dev:close()
        self._dev = nil
    end
    if self._owns_bus and self._bus then
        self._bus:close()
        self._bus = nil
    end
end

function mt:__gc()
    pcall(function()
        self:close()
    end)
end

return M
