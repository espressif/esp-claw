-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
--
-- Pure-Lua driver for the PY32 based M5Stack I2C IO expander (14 GPIO, 4 ADC
-- channels, 4 PWM channels and an addressable-LED RAM block). Register map
-- mirrors the M5Stack PY32IOExpander reference driver, routed through the
-- builtin `i2c` module so one .lua file works on any board.

local i2c = require("i2c")
local delay = require("delay")

local M = {}

local DEFAULT_FREQ_HZ = 100000
local DEFAULT_ADDR = 0x6F               -- ADD_SEL=GND; the only documented address
local PIN_COUNT = 14                    -- P0..P13
local PINS_PER_REG = 8
-- Each GPIO field is a consecutive _L/_H register pair (e.g. mode is 0x03/0x04),
-- so the high half is one register up. PWM duty channels are a different
-- layout: two registers each (0x1B/0x1C, 0x1D/0x1E, ...).
local GPIO_PAIR_STRIDE = 1
local PWM_CHANNEL_STRIDE = 2
local ADC_CHANNELS = 4
local PWM_CHANNELS = 4
local LED_MAX = 32
-- IO14 (logical pin 13) is muxed: its default function is GPIO, and the
-- Neopixel output is the alternate. The chip datasheet and the vendor M5IOE1
-- library treat the two as mutually exclusive, so this driver refuses GPIO calls
-- on the pin while the LED block is armed. Note that on the StackChan unit this
-- was tested against, the strip lights either way -- the exclusion is what the
-- documentation specifies, not something observed to break.
local NEOPIXEL_PIN = 13
local PWM_DUTY_MAX = 4095
local BOOT_POLL_INTERVAL_MS = 100
local DEFAULT_BOOT_TIMEOUT_MS = 1200    -- the PY32 boots slower than the host SoC

-- PY32 register map. GPIO registers come in _L (P0..P7) / _H (P8..P13) pairs.
local REG_UID_L      = 0x00
local REG_VERSION    = 0x02
local REG_GPIO_M_L   = 0x03             -- mode: 0 = input, 1 = output
local REG_GPIO_O_L   = 0x05             -- output level
local REG_GPIO_I_L   = 0x07             -- input level
local REG_GPIO_PU_L  = 0x09             -- pull-up enable
local REG_GPIO_PD_L  = 0x0B             -- pull-down enable
local REG_GPIO_IE_L  = 0x0D             -- interrupt enable
local REG_GPIO_IS_L  = 0x11             -- interrupt status (write 1 to clear)
local REG_GPIO_DRV_L = 0x13             -- 0 = push-pull, 1 = open-drain

local REG_ADC_CTRL   = 0x15             -- bit7 busy, bit6 start, bits2:0 channel
local REG_ADC_D_L    = 0x16

local REG_PWM1_DUTY_L = 0x1B            -- channels are 2 registers apart
local REG_LED_CFG     = 0x24            -- bits5:0 led count, bit6 refresh
local REG_PWM_FREQ_L  = 0x25
local REG_LED_RAM     = 0x30            -- 2 bytes (RGB565, little endian) per LED

local ADC_BUSY = 1 << 7
local ADC_START = 1 << 6
local LED_REFRESH_BIT = 6               -- LED_CFG bit 6, per the chip datasheet
local PWM_ENABLE = 1 << 7

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- Argument checking
-- ---------------------------------------------------------------------------

local function check_int(value, name, min, max)
    if type(value) ~= "number" then
        error(string.format("py32_ioe: %s must be a number", name))
    end
    value = math.floor(value)
    if value < min or value > max then
        error(string.format("py32_ioe: %s %d out of range %d..%d", name, value, min, max))
    end
    return value
end

local function check_pin(pin)
    return check_int(pin, "pin", 0, PIN_COUNT - 1)
end

local function check_byte(value, name)
    return check_int(value, name, 0, 255)
end

-- ---------------------------------------------------------------------------
-- Register access. `reg_low` names the _L half; pins 8..13 live in _H.
--
-- Burst access is only valid *within* the chip's register blocks
-- (0x00-0x2F, 0x30-0x6F, 0x70-0x8F, 0x90); every multi-byte access below stays
-- inside one block.
-- ---------------------------------------------------------------------------

local function pin_reg(reg_low, pin)
    if pin < PINS_PER_REG then
        return reg_low, 1 << pin
    end
    return reg_low + GPIO_PAIR_STRIDE, 1 << (pin - PINS_PER_REG)
end

local function write_pin_bit(dev, reg_low, pin, value)
    local reg, mask = pin_reg(reg_low, pin)
    local current = dev:read_byte(reg)
    local updated
    if value then
        updated = current | mask
    else
        updated = current & ~mask & 0xFF
    end
    if updated ~= current then
        dev:write_byte(updated, reg)
    end
end

local function read_pin_bit(dev, reg_low, pin)
    local reg, mask = pin_reg(reg_low, pin)
    return (dev:read_byte(reg) & mask) ~= 0
end

local function read_u16(dev, reg_low)
    local raw = dev:read(2, reg_low)
    return (string.byte(raw, 1) or 0) | ((string.byte(raw, 2) or 0) << 8)
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
            assert(opts.port, "py32_ioe.new: missing 'port'"),
            assert(opts.sda,  "py32_ioe.new: missing 'sda'"),
            assert(opts.scl,  "py32_ioe.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = opts.close_bus == true
    end

    local addr = check_int(opts.addr or DEFAULT_ADDR, "addr", 0x08, 0x77)
    return bus, bus:device(addr, 0), addr, owns_bus
end

-- The PY32 NAKs or reports 0x00/0xFF until its firmware is running, so poll the
-- version register instead of trusting the first read.
local function wait_ready(dev, timeout_ms)
    local waited = 0
    while true do
        local ok, version = pcall(dev.read_byte, dev, REG_VERSION)
        if ok and version ~= 0x00 and version ~= 0xFF then
            return version
        end
        if waited >= timeout_ms then
            error(string.format("py32_ioe: no valid version register after %d ms", waited))
        end
        delay.delay_ms(BOOT_POLL_INTERVAL_MS)
        waited = waited + BOOT_POLL_INTERVAL_MS
    end
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local bus, dev, addr, owns_bus = new_device_from_opts(opts)

    local timeout_ms = opts.boot_timeout_ms or DEFAULT_BOOT_TIMEOUT_MS
    local version = wait_ready(dev, check_int(timeout_ms, "boot_timeout_ms", 0, 60000))

    return setmetatable({
        _bus = bus,
        _dev = dev,
        _addr = addr,
        _owns_bus = owns_bus,
        _version = version,
    }, mt)
end

function M.pin_count()
    return PIN_COUNT
end

function M.led_max()
    return LED_MAX
end

-- ---------------------------------------------------------------------------
-- Identity
-- ---------------------------------------------------------------------------

function mt:address()
    return self._addr
end

function mt:version()
    return self._dev:read_byte(REG_VERSION)
end

function mt:uid()
    return read_u16(self._dev, REG_UID_L)
end

-- ---------------------------------------------------------------------------
-- GPIO
-- ---------------------------------------------------------------------------

-- Refuse GPIO use of the Neopixel pin while the LED block is armed. This is the
-- constraint the datasheet and the vendor library state; it also turns a
-- confusing silent failure into an error message.
local function assert_pin_free(self, pin)
    if pin ~= NEOPIXEL_PIN then
        return
    end
    local count = self._dev:read_byte(REG_LED_CFG) & 0x3F
    if count ~= 0 then
        error(string.format(
            "py32_ioe: pin %d is driving %d Neopixel LEDs; call disable_leds() before using it as GPIO",
            NEOPIXEL_PIN, count))
    end
end

function mt:set_dir(pin, is_output)
    pin = check_pin(pin)
    assert_pin_free(self, pin)
    write_pin_bit(self._dev, REG_GPIO_M_L, pin, is_output and true or false)
end

-- `pull` is "up", "down" or "none".
function mt:set_pull(pin, pull)
    pin = check_pin(pin)
    assert_pin_free(self, pin)
    if pull == "up" then
        write_pin_bit(self._dev, REG_GPIO_PD_L, pin, false)
        write_pin_bit(self._dev, REG_GPIO_PU_L, pin, true)
    elseif pull == "down" then
        write_pin_bit(self._dev, REG_GPIO_PU_L, pin, false)
        write_pin_bit(self._dev, REG_GPIO_PD_L, pin, true)
    elseif pull == "none" or pull == nil then
        write_pin_bit(self._dev, REG_GPIO_PU_L, pin, false)
        write_pin_bit(self._dev, REG_GPIO_PD_L, pin, false)
    else
        error('py32_ioe: pull must be "up", "down" or "none"')
    end
end

-- `open_drain = false` selects push-pull, which is what LED data lines need.
function mt:set_drive(pin, open_drain)
    pin = check_pin(pin)
    assert_pin_free(self, pin)
    write_pin_bit(self._dev, REG_GPIO_DRV_L, pin, open_drain and true or false)
end

function mt:write(pin, level)
    pin = check_pin(pin)
    assert_pin_free(self, pin)
    write_pin_bit(self._dev, REG_GPIO_O_L, pin, level and true or false)
end

function mt:read(pin)
    return read_pin_bit(self._dev, REG_GPIO_I_L, check_pin(pin))
end

function mt:read_output(pin)
    return read_pin_bit(self._dev, REG_GPIO_O_L, check_pin(pin))
end

function mt:set_irq_enabled(pin, enabled)
    write_pin_bit(self._dev, REG_GPIO_IE_L, check_pin(pin), enabled and true or false)
end

function mt:clear_irq()
    -- Writing 1s clears latched status; only bits 0..5 of the high half exist.
    self._dev:write_byte(0xFF, REG_GPIO_IS_L)
    self._dev:write_byte(0x3F, REG_GPIO_IS_L + GPIO_PAIR_STRIDE)
end

-- ---------------------------------------------------------------------------
-- ADC. Channels are 1..4; returns the raw conversion result.
-- ---------------------------------------------------------------------------

function mt:analog_read(channel)
    channel = check_int(channel, "channel", 1, ADC_CHANNELS)
    local dev = self._dev
    dev:write_byte(ADC_START | channel, REG_ADC_CTRL)

    for _ = 1, 100 do
        if (dev:read_byte(REG_ADC_CTRL) & ADC_BUSY) == 0 then
            return read_u16(dev, REG_ADC_D_L)
        end
        delay.delay_ms(1)
    end
    error(string.format("py32_ioe: ADC channel %d conversion timed out", channel))
end

-- ---------------------------------------------------------------------------
-- PWM. Channels are 0..3; duty is 0..255 and scaled to the 12-bit hardware.
-- ---------------------------------------------------------------------------

function mt:set_pwm_duty(channel, duty)
    channel = check_int(channel, "channel", 0, PWM_CHANNELS - 1)
    duty = check_byte(duty, "duty")

    local duty12 = duty * 16
    if duty12 > PWM_DUTY_MAX then
        duty12 = PWM_DUTY_MAX
    end

    local reg_low = REG_PWM1_DUTY_L + channel * PWM_CHANNEL_STRIDE
    self._dev:write_byte(duty12 & 0xFF, reg_low)
    self._dev:write_byte(((duty12 >> 8) & 0x0F) | PWM_ENABLE, reg_low + 1)
end

function mt:set_pwm_frequency(freq_hz)
    freq_hz = check_int(freq_hz, "freq_hz", 0, 65535)
    self._dev:write(string.char(freq_hz & 0xFF, (freq_hz >> 8) & 0xFF), REG_PWM_FREQ_L)
end

-- ---------------------------------------------------------------------------
-- Addressable LEDs on IO14 (pin 13).
--
-- The vendor sequence is only three steps and deliberately does NOT configure
-- the pin: set the count, write LED RAM, set the refresh flag. Configuring pin
-- 13 as a GPIO takes the pin away from the LED engine, so this library refuses
-- that combination rather than letting it fail silently.
-- ---------------------------------------------------------------------------

local function rgb_to_565(r, g, b)
    r = check_byte(r, "r")
    g = check_byte(g, "g")
    b = check_byte(b, "b")
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
end

M.rgb_to_565 = rgb_to_565
M.NEOPIXEL_PIN = NEOPIXEL_PIN

-- The expander is a separate MCU on its own rail, so its register state
-- survives a host reset: whatever a previous session left in IO14's mode, pull
-- and drive bits is still there after re-flashing. Arming the strip therefore
-- restores IO14 to its reset default (input, no pulls, push-pull) so the LED
-- engine always starts from a known state.
local function release_neopixel_pin(dev)
    write_pin_bit(dev, REG_GPIO_M_L, NEOPIXEL_PIN, false)
    write_pin_bit(dev, REG_GPIO_PU_L, NEOPIXEL_PIN, false)
    write_pin_bit(dev, REG_GPIO_PD_L, NEOPIXEL_PIN, false)
    write_pin_bit(dev, REG_GPIO_DRV_L, NEOPIXEL_PIN, false)
end

function mt:set_led_count(count)
    count = check_int(count, "count", 0, LED_MAX)
    if count > 0 then
        release_neopixel_pin(self._dev)
    end
    -- Bits 0..5 are the count; bit 6 is a one-shot refresh flag.
    self._dev:write_byte(count & 0x3F, REG_LED_CFG)
    self._led_count = count
end

-- Count 0 means "all off" and releases pin 13 for GPIO use again.
function mt:disable_leds()
    self:set_led_count(0)
end

-- Escape hatch. The chip datasheet and the vendor M5IOE1 library treat IO14's
-- GPIO function and its Neopixel output as mutually exclusive, which is why
-- set_led_count releases the pin. The older M5Stack PY32IOExpander BSP does the
-- opposite: it configures the pin as a push-pull output with a pull-up and then
-- enables the LED block. If a board only lights up that way, call this after
-- set_led_count. Bypasses the mutual-exclusion guard by design.
function mt:force_neopixel_pin_output()
    write_pin_bit(self._dev, REG_GPIO_M_L, NEOPIXEL_PIN, true)
    write_pin_bit(self._dev, REG_GPIO_PD_L, NEOPIXEL_PIN, false)
    write_pin_bit(self._dev, REG_GPIO_PU_L, NEOPIXEL_PIN, true)
    write_pin_bit(self._dev, REG_GPIO_DRV_L, NEOPIXEL_PIN, false)
end

function mt:led_count()
    return self._led_count or (self._dev:read_byte(REG_LED_CFG) & 0x3F)
end

function mt:set_led_color565(index, color565)
    index = check_int(index, "index", 0, LED_MAX - 1)
    color565 = check_int(color565, "color565", 0, 0xFFFF)
    self._dev:write(
        string.char(color565 & 0xFF, (color565 >> 8) & 0xFF),
        REG_LED_RAM + index * 2)
end

function mt:set_led_color(index, r, g, b)
    self:set_led_color565(index, rgb_to_565(r, g, b))
end

-- Read a staged colour back out of LED RAM. Useful to prove the chip accepted
-- the write when the strip stays dark.
function mt:read_led_color565(index)
    index = check_int(index, "index", 0, LED_MAX - 1)
    local raw = self._dev:read(2, REG_LED_RAM + index * 2)
    return (string.byte(raw, 1) or 0) | ((string.byte(raw, 2) or 0) << 8)
end

-- Raw LED_CFG access. The count lives in the low bits and the refresh flag is a
-- one-shot the chip clears after driving the strip, so reading this back tells
-- you whether the LED engine actually ran.
function mt:led_config_raw()
    return self._dev:read_byte(REG_LED_CFG)
end

function mt:set_led_config_raw(value)
    self._dev:write_byte(check_byte(value, "value"), REG_LED_CFG)
end

-- Trigger the LED engine. The chip auto-clears the flag once it has driven the
-- strip, so a read-back of bit 6 tells you whether the engine ran.
function mt:refresh_leds()
    local cfg = self._dev:read_byte(REG_LED_CFG)
    self._dev:write_byte(cfg | (1 << LED_REFRESH_BIT), REG_LED_CFG)
end

-- Set every configured LED to one colour and flush in a single refresh.
function mt:fill_leds(r, g, b)
    local color = rgb_to_565(r, g, b)
    for index = 0, self:led_count() - 1 do
        self:set_led_color565(index, color)
    end
    self:refresh_leds()
end

-- ---------------------------------------------------------------------------
-- Teardown
-- ---------------------------------------------------------------------------

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
