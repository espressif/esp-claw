-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
--
-- Pure-Lua driver for the TI INA226 bidirectional current/power monitor.
-- Calibration and scaling follow the M5Stack INA226 reference driver, routed
-- through the builtin `i2c` module so one .lua file works on any board.

local i2c = require("i2c")
local delay = require("delay")

local M = {}

local DEFAULT_FREQ_HZ = 400000
local DEFAULT_ADDR = 0x40               -- A1=A0=GND. StackChan wires it to 0x41.
local DIE_ID = 0x2260
local MANUFACTURER_ID = 0x5449          -- "TI"

-- Register map (all 16-bit, big endian on the wire).
local REG_CONFIG      = 0x00
local REG_SHUNT_V     = 0x01
local REG_BUS_V       = 0x02
local REG_POWER       = 0x03
local REG_CURRENT     = 0x04
local REG_CALIBRATION = 0x05
local REG_MASK_ENABLE = 0x06
local REG_MANUFACTURER_ID = 0xFE
local REG_DIE_ID      = 0xFF

-- CONFIG bits 14:12 are reserved and specified as 0b100; keep them set so the
-- written value matches what the part reads back.
local CONFIG_RESERVED = 0x4000
local MASK_ENABLE_CVRF = 0x0008         -- conversion ready flag

-- Fixed LSB weights from the datasheet.
local BUS_VOLTAGE_LSB_V = 0.00125       -- 1.25 mV
local SHUNT_VOLTAGE_LSB_V = 0.0000025   -- 2.5 uV
local POWER_LSB_PER_CURRENT_LSB = 25.0
local CURRENT_LSB_DIVISOR = 32768.0
local CALIBRATION_CONSTANT = 0.00512

-- Averaging (CONFIG bits 11:9): samples averaged per conversion.
M.AVERAGING = {
    [1] = 0, [4] = 1, [16] = 2, [64] = 3,
    [128] = 4, [256] = 5, [512] = 6, [1024] = 7,
}

-- Conversion time in microseconds (CONFIG bits 8:6 for bus, 5:3 for shunt).
M.CONVERSION_TIME_US = {
    [140] = 0, [204] = 1, [332] = 2, [588] = 3,
    [1100] = 4, [2116] = 5, [4156] = 6, [8244] = 7,
}

-- Operating mode (CONFIG bits 2:0).
M.MODE = {
    power_down = 0,
    shunt_triggered = 1,
    bus_triggered = 2,
    shunt_bus_triggered = 3,
    shunt_continuous = 5,
    bus_continuous = 6,
    shunt_bus_continuous = 7,
}

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- Register access
-- ---------------------------------------------------------------------------

local function read_u16(dev, reg)
    local raw = dev:read(2, reg)
    return ((string.byte(raw, 1) or 0) << 8) | (string.byte(raw, 2) or 0)
end

local function read_s16(dev, reg)
    local value = read_u16(dev, reg)
    if value >= 0x8000 then
        value = value - 0x10000
    end
    return value
end

local function write_u16(dev, reg, value)
    value = value & 0xFFFF
    dev:write(string.char((value >> 8) & 0xFF, value & 0xFF), reg)
end

-- ---------------------------------------------------------------------------
-- Option decoding
-- ---------------------------------------------------------------------------

-- Accept only the documented human values ("1024" samples, "1100" us). Raw
-- field codes are rejected because 1 and 4 are valid sample counts *and* valid
-- codes, so accepting both would be ambiguous.
local function encode_option(value, table_by_value, name)
    if value == nil then
        return nil
    end
    if type(value) ~= "number" then
        error(string.format("ina226: %s must be a number", name))
    end
    local code = table_by_value[value]
    if code == nil then
        error(string.format("ina226: unsupported %s value %s", name, tostring(value)))
    end
    return code
end

local function encode_mode(value)
    if value == nil then
        return nil
    end
    if type(value) == "string" then
        local code = M.MODE[value]
        if not code then
            error(string.format('ina226: unknown mode "%s"', value))
        end
        return code
    end
    if type(value) == "number" and value >= 0 and value <= 7 then
        return math.floor(value)
    end
    error("ina226: mode must be a name or 0..7")
end

-- ---------------------------------------------------------------------------
-- Configuration. current_lsb sets the resolution of the CURRENT and POWER
-- registers; the calibration register maps it onto the actual shunt.
-- ---------------------------------------------------------------------------

local function apply_config(self, cfg)
    local averaging = encode_option(cfg.averaging, M.AVERAGING, "averaging")
        or self._averaging
    local bus_ct = encode_option(cfg.bus_conversion_time_us, M.CONVERSION_TIME_US,
        "bus_conversion_time_us") or self._bus_ct
    local shunt_ct = encode_option(cfg.shunt_conversion_time_us, M.CONVERSION_TIME_US,
        "shunt_conversion_time_us") or self._shunt_ct
    local mode = encode_mode(cfg.mode) or self._mode

    local shunt_res = cfg.shunt_res or self._shunt_res
    local max_current = cfg.max_expected_current or self._max_current
    if type(shunt_res) ~= "number" or shunt_res <= 0 then
        error("ina226: shunt_res must be a positive number of ohms")
    end
    if type(max_current) ~= "number" or max_current <= 0 then
        error("ina226: max_expected_current must be a positive number of amps")
    end

    local config_value = CONFIG_RESERVED
        | (averaging << 9)
        | (bus_ct << 6)
        | (shunt_ct << 3)
        | mode
    write_u16(self._dev, REG_CONFIG, config_value)

    local current_lsb = max_current / CURRENT_LSB_DIVISOR
    local calibration = math.floor(CALIBRATION_CONSTANT / (current_lsb * shunt_res))
    if calibration < 1 or calibration > 0xFFFF then
        error(string.format(
            "ina226: calibration value %d out of range for shunt %g ohm and max current %g A",
            calibration, shunt_res, max_current))
    end
    write_u16(self._dev, REG_CALIBRATION, calibration)

    self._averaging = averaging
    self._bus_ct = bus_ct
    self._shunt_ct = shunt_ct
    self._mode = mode
    self._shunt_res = shunt_res
    self._max_current = max_current
    self._current_lsb = current_lsb
    self._calibration = calibration
end

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

local function new_device_from_opts(opts)
    local bus
    local owns_bus = false

    if opts.bus ~= nil then
        bus = opts.bus
    else
        bus = i2c.new(
            assert(opts.port, "ina226.new: missing 'port'"),
            assert(opts.sda,  "ina226.new: missing 'sda'"),
            assert(opts.scl,  "ina226.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = opts.close_bus == true
    end

    local addr = opts.addr or DEFAULT_ADDR
    if type(addr) ~= "number" or addr < 0x40 or addr > 0x4F then
        error(string.format("ina226: address 0x%02X outside the 0x40..0x4F range",
            type(addr) == "number" and addr or 0))
    end
    return bus, bus:device(addr, 0), addr, owns_bus
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local bus, dev, addr, owns_bus = new_device_from_opts(opts)

    -- There is no safe default for a shunt resistor: guessing produces
    -- plausible-looking but wrong amps, so require both values.
    assert(opts.shunt_res, "ina226.new: missing 'shunt_res' (ohms)")
    assert(opts.max_expected_current, "ina226.new: missing 'max_expected_current' (amps)")

    local self = setmetatable({
        _bus = bus,
        _dev = dev,
        _addr = addr,
        _owns_bus = owns_bus,
        _averaging = M.AVERAGING[16],
        _bus_ct = M.CONVERSION_TIME_US[1100],
        _shunt_ct = M.CONVERSION_TIME_US[1100],
        _mode = M.MODE.shunt_bus_continuous,
    }, mt)

    -- Identity gate is the manufacturer id, not the die id. Some parts M5Stack
    -- ships (StackChan's 0x41 among them) return the manufacturer id from the
    -- die-id register too, so gating on the die id rejects a working chip.
    -- `check_die_id` is accepted as an alias for the older option name.
    local check_id = opts.check_id
    if check_id == nil then
        check_id = opts.check_die_id
    end
    if check_id ~= false then
        local manufacturer = read_u16(dev, REG_MANUFACTURER_ID)
        if manufacturer ~= MANUFACTURER_ID then
            error(string.format(
                "ina226: unexpected manufacturer id 0x%04X at 0x%02X (expected 0x%04X)",
                manufacturer, addr, MANUFACTURER_ID))
        end
    end

    -- Recorded, never enforced: 0x2260 on a textbook INA226, something else on
    -- the variants that do not implement the register.
    self._die_id = read_u16(dev, REG_DIE_ID)

    apply_config(self, opts)
    return self
end

-- ---------------------------------------------------------------------------
-- Instance methods
-- ---------------------------------------------------------------------------

function mt:address()
    return self._addr
end

function mt:die_id()
    return read_u16(self._dev, REG_DIE_ID)
end

-- True when the die-id register holds the textbook INA226 value. False is not a
-- fault: several variants return the manufacturer id here instead.
function mt:die_id_is_standard()
    return self._die_id == DIE_ID
end

function mt:manufacturer_id()
    return read_u16(self._dev, REG_MANUFACTURER_ID)
end

function mt:current_lsb()
    return self._current_lsb
end

function mt:calibration()
    return self._calibration
end

function mt:configure(cfg)
    apply_config(self, type(cfg) == "table" and cfg or {})
end

-- Volts across the bus (VBUS pin to GND).
function mt:read_bus_voltage()
    return read_s16(self._dev, REG_BUS_V) * BUS_VOLTAGE_LSB_V
end

-- Volts across the shunt. Sign follows the current direction.
function mt:read_shunt_voltage()
    return read_s16(self._dev, REG_SHUNT_V) * SHUNT_VOLTAGE_LSB_V
end

-- Amps through the shunt, computed by the part from the calibration register.
function mt:read_current()
    return read_s16(self._dev, REG_CURRENT) * self._current_lsb
end

-- Watts. The power register is unsigned, so it never reports a sign.
function mt:read_power()
    return read_u16(self._dev, REG_POWER) * self._current_lsb * POWER_LSB_PER_CURRENT_LSB
end

function mt:read()
    return {
        bus_voltage = self:read_bus_voltage(),
        shunt_voltage = self:read_shunt_voltage(),
        current = self:read_current(),
        power = self:read_power(),
    }
end

-- Poll the conversion-ready flag. Useful after a triggered-mode write; in
-- continuous mode the registers are always readable.
function mt:wait_conversion_ready(timeout_ms)
    timeout_ms = timeout_ms or 100
    local waited = 0
    while true do
        if (read_u16(self._dev, REG_MASK_ENABLE) & MASK_ENABLE_CVRF) ~= 0 then
            return true
        end
        if waited >= timeout_ms then
            return false
        end
        delay.delay_ms(1)
        waited = waited + 1
    end
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
