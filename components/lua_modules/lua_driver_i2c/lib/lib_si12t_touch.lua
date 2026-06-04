-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
--
-- Pure-Lua driver for the Si12T 12-channel capacitive touch IC. Mirrors the
-- register sequence of the C reference driver but routes I2C through the
-- builtin `i2c` module so a single .lua file can be reused on any board.

local i2c = require("i2c")
local delay = require("delay")

local M = {}

local DEFAULT_FREQ_HZ = 100000
local DEFAULT_ADDR = 0x78               -- ID_SEL=VDD; use 0x68 for ID_SEL=GND
local DEFAULT_THRESHOLD = 3
local CHANNELS = 12
local CHANNEL_MASK_ALL = 0xFFF

-- Si12T register map (datasheet section 11.6).
local REG_SEN1     = 0x02
local REG_CFIG     = 0x08
local REG_CTRL     = 0x09
local REG_REF_RST1 = 0x0A
local REG_OUTPUT1  = 0x10

local CTRL_RESET   = 0x0F
local CTRL_RUN     = 0x07
local CFIG_DEFAULT = 0x22
local SRST_DELAY_MS = 20

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- channels: accept integer mask, array {1,2,3}, string "1,2,3", or nil/"all".
-- ---------------------------------------------------------------------------

local function channels_to_mask(spec)
    if spec == nil or spec == "all" then
        return CHANNEL_MASK_ALL
    end

    if type(spec) == "number" then
        local mask = math.floor(spec)
        if mask <= 0 or (mask & ~CHANNEL_MASK_ALL) ~= 0 then
            error(string.format(
                "si12t_touch: channels mask 0x%X invalid (allowed 0x001..0x%03X)",
                mask, CHANNEL_MASK_ALL))
        end
        return mask
    end

    local function add(mask, ch)
        local n = math.floor(tonumber(ch) or -1)
        if n < 1 or n > CHANNELS then
            error(string.format(
                "si12t_touch: channel %s out of range 1..%d", tostring(ch), CHANNELS))
        end
        return mask | (1 << (n - 1))
    end

    if type(spec) == "table" then
        local mask = 0
        for _, ch in ipairs(spec) do
            mask = add(mask, ch)
        end
        if mask == 0 then
            error("si12t_touch: channels list is empty")
        end
        return mask
    end

    if type(spec) == "string" then
        local mask = 0
        for n in string.gmatch(spec, "([^,%s]+)") do
            mask = add(mask, n)
        end
        if mask == 0 then
            error("si12t_touch: channels string is empty")
        end
        return mask
    end

    error("si12t_touch: channels must be number/table/string/nil")
end

-- ---------------------------------------------------------------------------
-- Bus / device construction (mirrors lib_fuel_gauge).
-- ---------------------------------------------------------------------------

local function new_device_from_opts(opts, default_addr)
    local bus
    local owns_bus = false

    if opts.bus ~= nil then
        bus = opts.bus
    else
        bus = i2c.new(
            assert(opts.port, "si12t_touch.new: missing 'port'"),
            assert(opts.sda,  "si12t_touch.new: missing 'sda'"),
            assert(opts.scl,  "si12t_touch.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = opts.close_bus == true
    end

    local addr = opts.addr or default_addr
    if addr ~= 0x68 and addr ~= 0x78 then
        error(string.format(
            "si12t_touch: unsupported i2c address 0x%02X (allowed 0x68 or 0x78)",
            addr))
    end
    return bus, bus:device(addr, 0), addr, owns_bus
end

-- ---------------------------------------------------------------------------
-- Register-level helpers and chip init.
-- ---------------------------------------------------------------------------

local function write_regs(dev, reg, bytes)
    dev:write(string.char(table.unpack(bytes)), reg)
end

local function chip_init(dev, threshold, channel_mask)
    local disable = (~channel_mask) & CHANNEL_MASK_ALL
    write_regs(dev, REG_REF_RST1, {
        0x00, 0x00,
        disable & 0xFF,
        (disable >> 8) & 0x0F,
        0x00, 0x00,
    })

    dev:write_byte(CTRL_RESET, REG_CTRL)
    delay.delay_ms(SRST_DELAY_MS)

    dev:write_byte(CTRL_RUN, REG_CTRL)
    delay.delay_ms(10)

    dev:write_byte(CFIG_DEFAULT, REG_CFIG)
    delay.delay_ms(5)

    -- SEN nibble is replicated, OR'd with 0x11 per the BSP reference.
    local thr = threshold & 0x07
    local sen_byte = ((thr << 4) | thr) | 0x11
    write_regs(dev, REG_SEN1, {
        sen_byte, sen_byte, sen_byte, sen_byte, sen_byte, sen_byte,
    })

    delay.delay_ms(200)

    -- Probe the OUTPUT registers so a missing chip surfaces here, not on the
    -- first user read.
    dev:read(3, REG_OUTPUT1)
end

local function read_status(dev, channel_mask)
    local raw = dev:read(3, REG_OUTPUT1)
    local b1 = string.byte(raw, 1) or 0
    local b2 = string.byte(raw, 2) or 0
    local b3 = string.byte(raw, 3) or 0
    local bytes = {b1, b2, b3}
    local s = 0
    for ch = 0, CHANNELS - 1 do
        local by = (ch >> 2) + 1            -- 1-based byte index
        local sh = (ch & 0x03) * 2
        if ((bytes[by] >> sh) & 0x03) ~= 0 then
            s = s | (1 << ch)
        end
    end
    return (s & CHANNEL_MASK_ALL) & channel_mask
end

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local bus, dev, addr, owns_bus = new_device_from_opts(opts, DEFAULT_ADDR)

    local threshold = opts.threshold or DEFAULT_THRESHOLD
    if type(threshold) ~= "number" or threshold < 0 or threshold > 7 then
        error("si12t_touch.new: threshold must be 0..7")
    end
    threshold = math.floor(threshold)

    local channel_mask = channels_to_mask(opts.channels)

    chip_init(dev, threshold, channel_mask)

    return setmetatable({
        _bus = bus,
        _dev = dev,
        _owns_bus = owns_bus,
        _addr = addr,
        _threshold = threshold,
        _channel_mask = channel_mask,
    }, mt)
end

function M.channels()
    return CHANNELS
end

function M.channel_mask_all()
    return CHANNEL_MASK_ALL
end

-- ---------------------------------------------------------------------------
-- Instance methods
-- ---------------------------------------------------------------------------

function mt:address()
    return self._addr
end

function mt:threshold()
    return self._threshold
end

function mt:channel_mask()
    return self._channel_mask
end

function mt:read()
    return read_status(self._dev, self._channel_mask)
end

function mt:read_channels()
    local s = self:read()
    local out = {}
    for i = 1, CHANNELS do
        out[i] = (s & (1 << (i - 1))) ~= 0
    end
    return out
end

function mt:set_threshold(value)
    if type(value) ~= "number" or value < 0 or value > 7 then
        error("si12t_touch: threshold must be 0..7")
    end
    value = math.floor(value)
    chip_init(self._dev, value, self._channel_mask)
    self._threshold = value
end

function mt:set_channels(spec)
    local mask = channels_to_mask(spec)
    chip_init(self._dev, self._threshold, mask)
    self._channel_mask = mask
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
