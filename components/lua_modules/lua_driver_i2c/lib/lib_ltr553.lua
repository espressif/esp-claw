-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
--
-- Pure-Lua driver for the Lite-On LTR-553ALS combined ambient light (ALS) and
-- proximity (PS) sensor, routed through the builtin `i2c` module so one .lua
-- file works on any board. This is the light sensor fitted to M5Stack CoreS3.

local i2c = require("i2c")
local delay = require("delay")

local M = {}

local DEFAULT_FREQ_HZ = 400000
local DEFAULT_ADDR = 0x23               -- fixed address, not strappable
local EXPECTED_PART_NUMBER = 0x9        -- PART_ID bits 7:4, default register value 0x92
local EXPECTED_MANUFAC_ID = 0x05

-- Register map.
local REG_ALS_CONTR     = 0x80          -- b0 active, b1 sw_reset, b2:4 gain
local REG_PS_CONTR      = 0x81          -- b1 active, b5 saturation indicator
local REG_PS_LED        = 0x82
local REG_PS_N_PULSES   = 0x83
local REG_PS_MEAS_RATE  = 0x84
local REG_MEAS_RATE     = 0x85          -- b0:2 repeat rate, b3:5 integration
local REG_PART_ID       = 0x86
local REG_MANUFAC_ID    = 0x87
local REG_ALS_DATA      = 0x88          -- ch1_lo, ch1_hi, ch0_lo, ch0_hi
local REG_ALS_PS_STATUS = 0x8C
local REG_PS_DATA       = 0x8D          -- lo, hi (hi: 3 data bits + b7 saturated)

local ALS_CONTR_ACTIVE = 1 << 0
local ALS_CONTR_RESET  = 1 << 1
local PS_CONTR_ACTIVE  = 1 << 1         -- PS mode bits 1:0; 0b10 selects active
local PS_CONTR_SATURATION_EN = 1 << 5

local STATUS_PS_NEW_DATA  = 1 << 0
local STATUS_PS_INTERRUPT = 1 << 1
local STATUS_ALS_NEW_DATA = 1 << 2
local STATUS_ALS_INTERRUPT = 1 << 3
local STATUS_DATA_INVALID = 1 << 7

local PS_DATA_SATURATED = 1 << 7
local PS_DATA_HIGH_MASK = 0x07          -- 11-bit result

local WAKEUP_DELAY_MS = 10              -- datasheet: standby to active
local RESET_DELAY_MS = 20

-- ALS gain multiplier -> register code. Codes 4 and 5 do not exist.
M.GAIN = {[1] = 0, [2] = 1, [4] = 2, [8] = 3, [48] = 6, [96] = 7}

-- ALS integration time in ms -> register code. Note the encoding is not
-- monotonic: 50 ms sits at code 1, between 100 ms and 200 ms.
M.INTEGRATION_TIME_MS = {
    [100] = 0, [50] = 1, [200] = 2, [400] = 3,
    [150] = 4, [250] = 5, [300] = 6, [350] = 7,
}

-- ALS measurement repeat rate in ms -> register code.
M.MEASUREMENT_RATE_MS = {[50] = 0, [100] = 1, [200] = 2, [500] = 3, [1000] = 4, [2000] = 5}

-- PS measurement rate in ms -> register code.
M.PS_MEASUREMENT_RATE_MS = {
    [50] = 0, [70] = 1, [100] = 2, [200] = 3,
    [500] = 4, [1000] = 5, [2000] = 6, [10] = 8,
}

-- PS gain multiplier -> register code (PS_CONTR bits 3:2).
M.PS_GAIN = {[16] = 0, [32] = 2, [64] = 3}

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- Option decoding. Only the documented human values are accepted; raw register
-- codes are rejected because several of them collide with valid ms values.
-- ---------------------------------------------------------------------------

local function encode_option(value, table_by_value, name)
    if type(value) ~= "number" then
        error(string.format("ltr553: %s must be a number", name))
    end
    local code = table_by_value[value]
    if code == nil then
        error(string.format("ltr553: unsupported %s value %s", name, tostring(value)))
    end
    return code
end

-- ---------------------------------------------------------------------------
-- Register access
-- ---------------------------------------------------------------------------

-- Both data blocks are little endian and must be read as one burst so the part
-- latches a consistent sample.
local function read_u16_le(dev, reg)
    local raw = dev:read(2, reg)
    return (string.byte(raw, 1) or 0) | ((string.byte(raw, 2) or 0) << 8)
end

local function write_als_contr(self)
    local value = (self._gain_code << 2)
    if self._als_enabled then
        value = value | ALS_CONTR_ACTIVE
    end
    self._dev:write_byte(value, REG_ALS_CONTR)
end

local function write_ps_contr(self)
    local value = self._ps_gain_code << 2
    if self._ps_saturation_indicator then
        value = value | PS_CONTR_SATURATION_EN
    end
    if self._ps_enabled then
        value = value | PS_CONTR_ACTIVE
    end
    self._dev:write_byte(value, REG_PS_CONTR)
end

local function write_meas_rate(self)
    -- The part silently clamps integration time to the repeat rate, which makes
    -- readings quietly disagree with the lux divisor. Refuse it instead.
    if self._integration_time_ms > self._rate_ms then
        error(string.format(
            "ltr553: integration_time_ms %d must be <= measurement_rate_ms %d",
            self._integration_time_ms, self._rate_ms))
    end
    self._dev:write_byte((self._integration_code << 3) | self._rate_code, REG_MEAS_RATE)
end

-- ---------------------------------------------------------------------------
-- Lux conversion, from the Lite-On "Using the Lux Equation" application note.
-- CH0 is visible + infrared, CH1 is infrared only; the ratio between them picks
-- which linear segment applies.
-- ---------------------------------------------------------------------------

local function compute_lux(ch0, ch1, gain, integration_time_ms)
    local total = ch0 + ch1
    if total == 0 then
        return 0.0
    end

    local ratio = ch1 / total
    local raw
    if ratio < 0.45 then
        raw = 1.7743 * ch0 + 1.1059 * ch1
    elseif ratio < 0.64 then
        raw = 4.2785 * ch0 - 1.9548 * ch1
    elseif ratio < 0.85 then
        raw = 0.5926 * ch0 + 0.1185 * ch1
    else
        -- Outside the characterised range; the note specifies zero here.
        return 0.0
    end

    local lux = raw / gain / (integration_time_ms / 100.0)
    if lux < 0.0 then
        return 0.0
    end
    return lux
end

M.compute_lux = compute_lux

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
            assert(opts.port, "ltr553.new: missing 'port'"),
            assert(opts.sda,  "ltr553.new: missing 'sda'"),
            assert(opts.scl,  "ltr553.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = opts.close_bus == true
    end

    local addr = opts.addr or DEFAULT_ADDR
    if type(addr) ~= "number" or addr < 0x08 or addr > 0x77 then
        error("ltr553: addr must be a 7-bit I2C address")
    end
    return bus, bus:device(addr, 0), addr, owns_bus
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local bus, dev, addr, owns_bus = new_device_from_opts(opts)

    local self = setmetatable({
        _bus = bus,
        _dev = dev,
        _addr = addr,
        _owns_bus = owns_bus,
        _gain = 1,
        _gain_code = M.GAIN[1],
        _integration_time_ms = 100,
        _integration_code = M.INTEGRATION_TIME_MS[100],
        _rate_ms = 500,
        _rate_code = M.MEASUREMENT_RATE_MS[500],
        _ps_gain = 16,
        _ps_gain_code = M.PS_GAIN[16],
        _ps_saturation_indicator = opts.ps_saturation_indicator ~= false,
        _als_enabled = opts.enable_als ~= false,
        _ps_enabled = opts.enable_ps == true,
    }, mt)

    if opts.check_id ~= false then
        local part_id = dev:read_byte(REG_PART_ID)
        local manufac_id = dev:read_byte(REG_MANUFAC_ID)
        if ((part_id >> 4) & 0x0F) ~= EXPECTED_PART_NUMBER or manufac_id ~= EXPECTED_MANUFAC_ID then
            error(string.format(
                "ltr553: unexpected ids at 0x%02X (part 0x%02X, manufacturer 0x%02X)",
                addr, part_id, manufac_id))
        end
    end

    if opts.gain ~= nil then
        self._gain = opts.gain
        self._gain_code = encode_option(opts.gain, M.GAIN, "gain")
    end
    if opts.integration_time_ms ~= nil then
        self._integration_time_ms = opts.integration_time_ms
        self._integration_code = encode_option(
            opts.integration_time_ms, M.INTEGRATION_TIME_MS, "integration_time_ms")
    end
    if opts.measurement_rate_ms ~= nil then
        self._rate_ms = opts.measurement_rate_ms
        self._rate_code = encode_option(
            opts.measurement_rate_ms, M.MEASUREMENT_RATE_MS, "measurement_rate_ms")
    end
    if opts.ps_gain ~= nil then
        self._ps_gain = opts.ps_gain
        self._ps_gain_code = encode_option(opts.ps_gain, M.PS_GAIN, "ps_gain")
    end

    write_meas_rate(self)
    if opts.ps_pulses ~= nil then
        self:set_ps_pulses(opts.ps_pulses)
    end
    if opts.ps_measurement_rate_ms ~= nil then
        self:set_ps_measurement_rate(opts.ps_measurement_rate_ms)
    end

    write_als_contr(self)
    write_ps_contr(self)
    if self._als_enabled or self._ps_enabled then
        delay.delay_ms(WAKEUP_DELAY_MS)
    end
    return self
end

-- ---------------------------------------------------------------------------
-- Identity and configuration
-- ---------------------------------------------------------------------------

function mt:address()
    return self._addr
end

function mt:part_id()
    local raw = self._dev:read_byte(REG_PART_ID)
    return (raw >> 4) & 0x0F, raw & 0x0F     -- part number (0x9), revision
end

function mt:manufacturer_id()
    return self._dev:read_byte(REG_MANUFAC_ID)
end

function mt:gain()
    return self._gain
end

function mt:integration_time_ms()
    return self._integration_time_ms
end

function mt:measurement_rate_ms()
    return self._rate_ms
end

function mt:set_gain(gain)
    self._gain_code = encode_option(gain, M.GAIN, "gain")
    self._gain = gain
    write_als_contr(self)
end

function mt:set_timing(integration_time_ms, measurement_rate_ms)
    local previous_integration = self._integration_time_ms
    local previous_integration_code = self._integration_code
    local previous_rate = self._rate_ms
    local previous_rate_code = self._rate_code

    if integration_time_ms ~= nil then
        self._integration_code = encode_option(
            integration_time_ms, M.INTEGRATION_TIME_MS, "integration_time_ms")
        self._integration_time_ms = integration_time_ms
    end
    if measurement_rate_ms ~= nil then
        self._rate_code = encode_option(
            measurement_rate_ms, M.MEASUREMENT_RATE_MS, "measurement_rate_ms")
        self._rate_ms = measurement_rate_ms
    end

    -- Roll back on rejection so the cached state keeps matching the chip.
    local ok, err = pcall(write_meas_rate, self)
    if not ok then
        self._integration_time_ms = previous_integration
        self._integration_code = previous_integration_code
        self._rate_ms = previous_rate
        self._rate_code = previous_rate_code
        error(err, 0)
    end
end

function mt:set_ps_gain(gain)
    self._ps_gain_code = encode_option(gain, M.PS_GAIN, "ps_gain")
    self._ps_gain = gain
    write_ps_contr(self)
end

function mt:ps_gain()
    return self._ps_gain
end

function mt:set_als_enabled(enabled)
    self._als_enabled = enabled and true or false
    write_als_contr(self)
    if self._als_enabled then
        delay.delay_ms(WAKEUP_DELAY_MS)
    end
end

function mt:set_ps_enabled(enabled)
    self._ps_enabled = enabled and true or false
    write_ps_contr(self)
    if self._ps_enabled then
        delay.delay_ms(WAKEUP_DELAY_MS)
    end
end

function mt:set_ps_pulses(count)
    if type(count) ~= "number" or count < 1 or count > 15 then
        error("ltr553: ps_pulses must be 1..15")
    end
    self._dev:write_byte(math.floor(count) & 0x0F, REG_PS_N_PULSES)
end

function mt:set_ps_measurement_rate(rate_ms)
    local code = encode_option(rate_ms, M.PS_MEASUREMENT_RATE_MS, "ps_measurement_rate_ms")
    self._dev:write_byte(code & 0x0F, REG_PS_MEAS_RATE)
end

-- LED pulse settings for the proximity emitter. Raw register value; see the
-- datasheet for the current / duty / frequency field layout.
function mt:set_ps_led_raw(value)
    if type(value) ~= "number" or value < 0 or value > 255 then
        error("ltr553: ps led register value must be 0..255")
    end
    self._dev:write_byte(math.floor(value), REG_PS_LED)
end

function mt:reset()
    self._dev:write_byte(ALS_CONTR_RESET, REG_ALS_CONTR)
    delay.delay_ms(RESET_DELAY_MS)
    -- Reset clears everything, so push our configuration back out.
    write_meas_rate(self)
    write_als_contr(self)
    write_ps_contr(self)
    if self._als_enabled or self._ps_enabled then
        delay.delay_ms(WAKEUP_DELAY_MS)
    end
end

-- ---------------------------------------------------------------------------
-- Measurements
-- ---------------------------------------------------------------------------

function mt:status()
    local raw = self._dev:read_byte(REG_ALS_PS_STATUS)
    return {
        raw = raw,
        ps_new_data = (raw & STATUS_PS_NEW_DATA) ~= 0,
        ps_interrupt = (raw & STATUS_PS_INTERRUPT) ~= 0,
        als_new_data = (raw & STATUS_ALS_NEW_DATA) ~= 0,
        als_interrupt = (raw & STATUS_ALS_INTERRUPT) ~= 0,
        gain_code = (raw >> 4) & 0x07,
        data_invalid = (raw & STATUS_DATA_INVALID) ~= 0,
    }
end

-- Returns ch0 (visible + infrared) and ch1 (infrared only). Read as one burst
-- starting at CH1 so the part hands back a consistent pair.
function mt:read_raw()
    local raw = self._dev:read(4, REG_ALS_DATA)
    local ch1 = (string.byte(raw, 1) or 0) | ((string.byte(raw, 2) or 0) << 8)
    local ch0 = (string.byte(raw, 3) or 0) | ((string.byte(raw, 4) or 0) << 8)
    return ch0, ch1
end

function mt:read_lux()
    local ch0, ch1 = self:read_raw()
    return compute_lux(ch0, ch1, self._gain, self._integration_time_ms), ch0, ch1
end

-- Returns the 11-bit proximity count and whether the reading saturated.
function mt:read_proximity()
    local raw = read_u16_le(self._dev, REG_PS_DATA)
    local high = (raw >> 8) & 0xFF
    local value = (raw & 0xFF) | ((high & PS_DATA_HIGH_MASK) << 8)
    return value, (high & PS_DATA_SATURATED) ~= 0
end

function mt:read()
    local status = self:status()
    local lux, ch0, ch1 = self:read_lux()
    local sample = {
        lux = lux,
        ch0 = ch0,
        ch1 = ch1,
        als_new_data = status.als_new_data,
        ps_new_data = status.ps_new_data,
        data_invalid = status.data_invalid,
    }
    if self._ps_enabled then
        sample.proximity, sample.proximity_saturated = self:read_proximity()
    end
    return sample
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
