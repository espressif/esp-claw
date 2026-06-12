local rmt = require("rmt")

local ok_gpio, gpio = pcall(require, "gpio")

local M = {}

local DEFAULT_NAME = "ir"
local DEFAULT_CARRIER_HZ = 38000
local DEFAULT_RESOLUTION_HZ = 1000000
local DEFAULT_TX_TIMEOUT_MS = 5000
local DEFAULT_RX_TIMEOUT_MS = 5000
local DEFAULT_RX_MIN_NS = 1250
local DEFAULT_RX_MAX_NS = 32000000
local DEFAULT_RX_MAX_SYMBOLS = 256

local NEC_LEADING_HIGH_US = 9000
local NEC_LEADING_LOW_US = 4500
local NEC_BIT_HIGH_US = 560
local NEC_ZERO_LOW_US = 560
local NEC_ONE_LOW_US = 1690
local NEC_ENDING_HIGH_US = 560

local IR = {}
IR.__index = IR

local function opt_int(opts, key, default)
    local value = opts[key]
    if type(value) ~= "number" then
        return default
    end
    return math.floor(value)
end

local function require_gpio_for_ctrl()
    if not ok_gpio then
        error("ir_driver: ctrl_gpio requires gpio driver")
    end
end

local function normalize_level(level)
    return level and level ~= 0 and 1 or 0
end

local function validate_symbol(symbol, index)
    if type(symbol) ~= "table" then
        error(string.format("ir_driver: symbol[%d] must be a table", index))
    end

    local duration0 = math.floor(assert(symbol.duration0, "ir_driver: missing duration0"))
    local duration1 = math.floor(assert(symbol.duration1, "ir_driver: missing duration1"))
    if duration0 < 0 or duration0 > 0x7FFF or duration1 < 0 or duration1 > 0x7FFF then
        error(string.format("ir_driver: symbol[%d] durations must be 0-32767", index))
    end

    return {
        level0 = symbol.level0 == nil and 1 or normalize_level(symbol.level0),
        duration0 = duration0,
        level1 = symbol.level1 == nil and 0 or normalize_level(symbol.level1),
        duration1 = duration1,
    }
end

local function copy_symbols(symbols)
    if type(symbols) ~= "table" then
        error("ir_driver: symbols must be a table")
    end
    local out = {}
    for i = 1, #symbols do
        out[i] = validate_symbol(symbols[i], i)
    end
    if #out == 0 then
        error("ir_driver: empty symbol list")
    end
    return out
end

local function maybe_invert_symbols(symbols, invert)
    if not invert then
        return symbols
    end
    local out = {}
    for i = 1, #symbols do
        local s = validate_symbol(symbols[i], i)
        out[i] = {
            level0 = s.level0 == 0 and 1 or 0,
            duration0 = s.duration0,
            level1 = s.level1 == 0 and 1 or 0,
            duration1 = s.duration1,
        }
    end
    return out
end

local function in_window(value, target, tolerance)
    return value >= target * (1 - tolerance) and value <= target * (1 + tolerance)
end

function M.build_nec(address, command)
    if type(address) ~= "number" or type(command) ~= "number" then
        error("ir_driver.build_nec: address and command must be numbers")
    end
    address = math.floor(address) & 0xFFFF
    command = math.floor(command) & 0xFFFF

    local payload = address | (command << 16)
    local symbols = {
        {
            level0 = 1,
            duration0 = NEC_LEADING_HIGH_US,
            level1 = 0,
            duration1 = NEC_LEADING_LOW_US,
        },
    }

    for bit = 0, 31 do
        local one = ((payload >> bit) & 0x1) ~= 0
        symbols[#symbols + 1] = {
            level0 = 1,
            duration0 = NEC_BIT_HIGH_US,
            level1 = 0,
            duration1 = one and NEC_ONE_LOW_US or NEC_ZERO_LOW_US,
        }
    end

    symbols[#symbols + 1] = {
        level0 = 1,
        duration0 = NEC_ENDING_HIGH_US,
        level1 = 0,
        duration1 = 0,
    }
    return symbols
end

function M.decode_nec(symbols, tolerance)
    tolerance = type(tolerance) == "number" and tolerance or 0.25
    symbols = copy_symbols(symbols)
    if #symbols < 33 then
        return nil, "too_short"
    end

    local leader = symbols[1]
    if not in_window(leader.duration0, NEC_LEADING_HIGH_US, tolerance) or
        not in_window(leader.duration1, NEC_LEADING_LOW_US, tolerance) then
        return nil, "bad_leader"
    end

    local payload = 0
    for bit = 0, 31 do
        local s = symbols[bit + 2]
        if not in_window(s.duration0, NEC_BIT_HIGH_US, tolerance) then
            return nil, "bad_bit_mark"
        end
        local is_zero = in_window(s.duration1, NEC_ZERO_LOW_US, tolerance)
        local is_one = in_window(s.duration1, NEC_ONE_LOW_US, tolerance)
        if not is_zero and not is_one then
            return nil, "bad_bit_space"
        end
        if is_one then
            payload = payload | (1 << bit)
        end
    end

    return {
        address = payload & 0xFFFF,
        command = (payload >> 16) & 0xFFFF,
        raw = payload,
    }
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}

    local self = setmetatable({
        _name = type(opts.name) == "string" and opts.name or DEFAULT_NAME,
        _tx_gpio = opt_int(opts, "tx_gpio", -1),
        _rx_gpio = opt_int(opts, "rx_gpio", -1),
        _ctrl_gpio = opt_int(opts, "ctrl_gpio", -1),
        _ctrl_active_level = opt_int(opts, "ctrl_active_level", 0),
        _carrier_hz = opt_int(opts, "carrier_hz", DEFAULT_CARRIER_HZ),
        _tx_resolution_hz = opt_int(opts, "tx_resolution_hz", DEFAULT_RESOLUTION_HZ),
        _rx_resolution_hz = opt_int(opts, "rx_resolution_hz", DEFAULT_RESOLUTION_HZ),
        _rx_max_symbols = opt_int(opts, "rx_max_symbols", DEFAULT_RX_MAX_SYMBOLS),
        _rx_min_ns = opt_int(opts, "signal_range_min_ns", DEFAULT_RX_MIN_NS),
        _rx_max_ns = opt_int(opts, "signal_range_max_ns", DEFAULT_RX_MAX_NS),
        _rx_invert = opts.rx_invert ~= false,
        _tx = nil,
        _rx = nil,
    }, IR)

    if self._tx_gpio < 0 and self._rx_gpio < 0 then
        error("ir_driver.new: at least one of tx_gpio or rx_gpio is required")
    end

    self:_assert_ctrl()
    return self
end

function IR:_assert_ctrl()
    if self._ctrl_gpio < 0 then
        return
    end
    require_gpio_for_ctrl()
    gpio.set_direction(self._ctrl_gpio, "output")
    gpio.set_level(self._ctrl_gpio, self._ctrl_active_level)
end

function IR:_ensure_tx()
    if self._tx then
        return self._tx
    end
    if self._tx_gpio < 0 then
        error("ir_driver: TX GPIO not configured")
    end
    self:_assert_ctrl()
    self._tx = rmt.tx({
        gpio = self._tx_gpio,
        resolution_hz = self._tx_resolution_hz,
        carrier_hz = self._carrier_hz,
    })
    return self._tx
end

function IR:_ensure_rx()
    if self._rx then
        return self._rx
    end
    if self._rx_gpio < 0 then
        error("ir_driver: RX GPIO not configured")
    end
    self:_assert_ctrl()
    self._rx = rmt.rx({
        gpio = self._rx_gpio,
        resolution_hz = self._rx_resolution_hz,
        max_symbols = self._rx_max_symbols,
        signal_range_min_ns = self._rx_min_ns,
        signal_range_max_ns = self._rx_max_ns,
    })
    return self._rx
end

function IR:name()
    return self._name
end

function IR:info()
    return {
        name = self._name,
        tx_gpio = self._tx_gpio,
        rx_gpio = self._rx_gpio,
        ctrl_gpio = self._ctrl_gpio,
        ctrl_active_level = self._ctrl_active_level,
        carrier_hz = self._carrier_hz,
        tx_resolution_hz = self._tx_resolution_hz,
        rx_resolution_hz = self._rx_resolution_hz,
        rx_max_symbols = self._rx_max_symbols,
        rx_invert = self._rx_invert,
    }
end

function IR:send_raw(symbols, timeout_ms)
    local tx = self:_ensure_tx()
    return tx:send(copy_symbols(symbols), timeout_ms or DEFAULT_TX_TIMEOUT_MS)
end

function IR:send_nec(address, command, timeout_ms)
    return self:send_raw(M.build_nec(address, command), timeout_ms)
end

function IR:start_receive()
    return self:_ensure_rx():start()
end

function IR:read_receive(timeout_ms)
    local symbols, err = self:_ensure_rx():read(timeout_ms or DEFAULT_RX_TIMEOUT_MS)
    if not symbols then
        return nil, err
    end
    return maybe_invert_symbols(symbols, self._rx_invert)
end

function IR:receive(timeout_ms)
    local symbols, err = self:_ensure_rx():receive(timeout_ms or DEFAULT_RX_TIMEOUT_MS)
    if not symbols then
        return nil, err
    end
    return maybe_invert_symbols(symbols, self._rx_invert)
end

function IR:receive_nec(timeout_ms, tolerance)
    local symbols, err = self:receive(timeout_ms)
    if not symbols then
        return nil, err
    end
    local decoded, decode_err = M.decode_nec(symbols, tolerance)
    if not decoded then
        return nil, decode_err, symbols
    end
    decoded.symbols = symbols
    return decoded
end

function IR:close()
    if self._tx then
        self._tx:close()
        self._tx = nil
    end
    if self._rx then
        self._rx:close()
        self._rx = nil
    end
end

function IR:__gc()
    pcall(function()
        self:close()
    end)
end

return M
