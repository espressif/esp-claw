-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
--
-- Pure-Lua driver for Feetech SCS serial-bus servos (SCSCL memory table),
-- routed through the builtin `uart` module so one .lua file works on any board.
-- Framing, checksums and byte order mirror the FTServo reference library.

local uart = require("uart")

local M = {}

local DEFAULT_BAUD = 1000000
local DEFAULT_TIMEOUT_MS = 20
local HEADER = "\xFF\xFF"
local HEADER_SCAN_MAX = 12              -- reference gives up after ~10 stray bytes
local BROADCAST_ID = 0xFE

-- Instructions.
local INST_PING       = 0x01
local INST_READ       = 0x02
local INST_WRITE      = 0x03
local INST_REG_WRITE  = 0x04
local INST_REG_ACTION = 0x05
local INST_SYNC_WRITE = 0x83

-- SCSCL memory table. EPROM entries survive a power cycle; SRAM entries do not.
local REG = {
    VERSION_L           = 3,    -- EPROM, read only
    ID                  = 5,    -- EPROM
    BAUD_RATE           = 6,    -- EPROM
    MIN_ANGLE_LIMIT_L   = 9,    -- EPROM
    MAX_ANGLE_LIMIT_L   = 11,   -- EPROM
    CW_DEAD             = 26,   -- EPROM
    CCW_DEAD            = 27,   -- EPROM
    TORQUE_ENABLE       = 40,   -- SRAM
    GOAL_POSITION_L     = 42,   -- SRAM
    GOAL_TIME_L         = 44,   -- SRAM
    GOAL_SPEED_L        = 46,   -- SRAM
    LOCK                = 48,   -- SRAM
    PRESENT_POSITION_L  = 56,   -- SRAM, read only
    PRESENT_SPEED_L     = 58,   -- SRAM, read only
    PRESENT_LOAD_L      = 60,   -- SRAM, read only
    PRESENT_VOLTAGE     = 62,   -- SRAM, read only
    PRESENT_TEMPERATURE = 63,   -- SRAM, read only
    MOVING              = 66,   -- SRAM, read only
    PRESENT_CURRENT_L   = 69,   -- SRAM, read only
}

M.REG = REG
M.BROADCAST_ID = BROADCAST_ID

-- Position mode drives GOAL_POSITION; PWM mode drives GOAL_TIME as a duty.
M.MODE_POSITION = 0
M.MODE_PWM = 1

local PWM_SIGN_BIT = 1 << 10            -- WritePWM encodes direction here
local LOAD_SIGN_BIT = 1 << 10
local SPEED_SIGN_BIT = 1 << 15
local CURRENT_SIGN_BIT = 1 << 15
local PWM_MAX = 1023
-- SCSCL encoder range. 1023 counts spans about 320 deg at 0.3125 deg/count.
local POS_MAX_DEFAULT = 1023

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- Argument checking. Bad arguments are programming errors and raise; bus
-- failures are expected at runtime and are returned as `nil, err`.
-- ---------------------------------------------------------------------------

local function check_id(id)
    if type(id) ~= "number" or id < 0 or id > 0xFF or math.floor(id) ~= id then
        error(string.format("scs_servo: id must be 0..255, got %s", tostring(id)))
    end
    return id
end

local function check_u16(value, name)
    if type(value) ~= "number" then
        error(string.format("scs_servo: %s must be a number", name))
    end
    value = math.floor(value)
    if value < 0 or value > 0xFFFF then
        error(string.format("scs_servo: %s %d out of range 0..65535", name, value))
    end
    return value
end

local function check_u8(value, name)
    if type(value) ~= "number" then
        error(string.format("scs_servo: %s must be a number", name))
    end
    value = math.floor(value)
    if value < 0 or value > 0xFF then
        error(string.format("scs_servo: %s %d out of range 0..255", name, value))
    end
    return value
end

-- ---------------------------------------------------------------------------
-- Framing. SCSCL sends 16-bit values high byte first.
-- ---------------------------------------------------------------------------

local function split_u16(value)
    return (value >> 8) & 0xFF, value & 0xFF
end

local function join_u16(high, low)
    return ((high & 0xFF) << 8) | (low & 0xFF)
end

-- Apply a sign-magnitude bit: the servo reports direction as a flag, not as
-- two's complement.
local function apply_sign(value, sign_bit)
    if (value & sign_bit) ~= 0 then
        return -(value & ~sign_bit)
    end
    return value
end

local function build_frame(id, inst, mem_addr, params)
    mem_addr = mem_addr or 0
    local bytes
    local msg_len

    if params then
        -- msg_len covers instruction + mem_addr + params + checksum.
        msg_len = #params + 3
        bytes = {id, msg_len, inst, mem_addr}
        for i = 1, #params do
            bytes[#bytes + 1] = params[i]
        end
    else
        msg_len = 2
        bytes = {id, msg_len, inst}
    end

    -- mem_addr is folded into the checksum even when it is not transmitted,
    -- which is harmless because those frames always pass 0.
    local sum = id + msg_len + inst + mem_addr
    if params then
        for i = 1, #params do
            sum = sum + params[i]
        end
    end

    bytes[#bytes + 1] = (~sum) & 0xFF
    return HEADER .. string.char(table.unpack(bytes))
end

-- ---------------------------------------------------------------------------
-- Transport
-- ---------------------------------------------------------------------------

-- Scan for the 0xFF 0xFF preamble, tolerating a little line noise.
local function find_header(u, timeout_ms)
    local prev = -1
    for _ = 1, HEADER_SCAN_MAX do
        local chunk = u:read(1, timeout_ms)
        if chunk == nil or #chunk == 0 then
            return false
        end
        local byte = string.byte(chunk)
        if prev == 0xFF and byte == 0xFF then
            return true
        end
        prev = byte
    end
    return false
end

local function read_exact(u, len, timeout_ms)
    local chunk = u:read(len, timeout_ms)
    if chunk == nil or #chunk ~= len then
        return nil
    end
    return chunk
end

-- Read a status packet with `data_len` payload bytes.
-- Returns payload string, status byte -- or nil, err.
local function read_status_packet(self, id, data_len)
    local u = self._uart
    local timeout_ms = self._timeout_ms

    if not find_header(u, timeout_ms) then
        return nil, "no reply"
    end

    local head = read_exact(u, 3, timeout_ms)      -- id, length, status
    if head == nil then
        return nil, "no reply"
    end

    local reply_id = string.byte(head, 1)
    local reply_len = string.byte(head, 2)
    local status = string.byte(head, 3)

    if reply_id ~= id and id ~= BROADCAST_ID then
        return nil, string.format("id mismatch: expected %d, got %d", id, reply_id)
    end
    if reply_len ~= data_len + 2 then
        return nil, string.format("length mismatch: expected %d, got %d", data_len + 2, reply_len)
    end

    local payload = ""
    if data_len > 0 then
        payload = read_exact(u, data_len, timeout_ms)
        if payload == nil then
            return nil, "truncated payload"
        end
    end

    local tail = read_exact(u, 1, timeout_ms)
    if tail == nil then
        return nil, "missing checksum"
    end

    local sum = reply_id + reply_len + status
    for i = 1, data_len do
        sum = sum + string.byte(payload, i)
    end
    if ((~sum) & 0xFF) ~= string.byte(tail) then
        return nil, "checksum mismatch"
    end

    return payload, status
end

local function transact(self, id, inst, mem_addr, params, data_len)
    local u = self._uart
    u:flush_input()
    u:write(build_frame(id, inst, mem_addr, params))

    -- Broadcast frames are never acknowledged.
    if id == BROADCAST_ID then
        return "", 0
    end
    return read_status_packet(self, id, data_len)
end

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

function M.new(opts)
    opts = type(opts) == "table" and opts or {}

    local u
    local owns_uart = false
    if opts.uart ~= nil then
        u = opts.uart
    else
        u = uart.new(
            assert(opts.port, "scs_servo.new: missing 'port'"),
            assert(opts.tx, "scs_servo.new: missing 'tx'"),
            assert(opts.rx, "scs_servo.new: missing 'rx'"),
            opts.baud or DEFAULT_BAUD
        )
        owns_uart = opts.close_uart ~= false
    end

    local self_obj = setmetatable({
        _uart = u,
        _owns_uart = owns_uart,
        _timeout_ms = opts.timeout_ms or DEFAULT_TIMEOUT_MS,
        -- SwitchMode has to stash the position-mode angle limits before it
        -- zeroes them, so PWM mode can be undone.
        _angle_limits = {},
        -- Per-id travel limits, plus a bus-wide fallback that keeps commands
        -- inside the encoder's own range.
        _pos_limits = {},
        _default_limits = {0, check_u16(opts.pos_max or POS_MAX_DEFAULT, "pos_max")},
    }, mt)

    if type(opts.pos_limits) == "table" then
        for id, limits in pairs(opts.pos_limits) do
            self_obj:set_pos_limits(id, limits[1] or limits.min, limits[2] or limits.max)
        end
    end

    return self_obj
end

-- ---------------------------------------------------------------------------
-- Raw register access
-- ---------------------------------------------------------------------------

function mt:write_bytes(id, mem_addr, bytes)
    check_id(id)
    check_u8(mem_addr, "mem_addr")
    local ok, err = transact(self, id, INST_WRITE, mem_addr, bytes, 0)
    if ok == nil then
        return nil, err
    end
    return true
end

function mt:write_byte(id, mem_addr, value)
    return self:write_bytes(id, mem_addr, {check_u8(value, "value")})
end

function mt:write_word(id, mem_addr, value)
    local high, low = split_u16(check_u16(value, "value"))
    return self:write_bytes(id, mem_addr, {high, low})
end

function mt:read_bytes(id, mem_addr, len)
    check_id(id)
    check_u8(mem_addr, "mem_addr")
    check_u8(len, "len")
    local payload, err = transact(self, id, INST_READ, mem_addr, {len}, len)
    if payload == nil then
        return nil, err
    end
    return payload
end

function mt:read_byte(id, mem_addr)
    local payload, err = self:read_bytes(id, mem_addr, 1)
    if payload == nil then
        return nil, err
    end
    return string.byte(payload, 1)
end

function mt:read_word(id, mem_addr)
    local payload, err = self:read_bytes(id, mem_addr, 2)
    if payload == nil then
        return nil, err
    end
    return join_u16(string.byte(payload, 1), string.byte(payload, 2))
end

-- ---------------------------------------------------------------------------
-- Discovery
-- ---------------------------------------------------------------------------

-- Returns the responding id, or nil, err.
function mt:ping(id)
    check_id(id)
    self._uart:flush_input()
    self._uart:write(build_frame(id, INST_PING, 0, nil))

    local payload, err = read_status_packet(self, id, 0)
    if payload == nil then
        return nil, err
    end
    return id
end

-- Sweep ids 0..253 and return an array of the ones that answer.
function mt:scan(first, last)
    first = first or 0
    last = last or 253
    local found = {}
    for id = first, last do
        if self:ping(id) then
            found[#found + 1] = id
        end
    end
    return found
end

-- ---------------------------------------------------------------------------
-- Position limits
--
-- The SCSCL encoder spans 0..1023 counts (about 320 deg at 0.3125 deg/count),
-- but the machine a servo is bolted into usually allows less. Nothing in the
-- protocol stops a caller from commanding a position past the mechanical stop,
-- where the servo stalls, draws heavy current and heats up. Limits are per id
-- because they are a property of the machine, not of the servo.
--
-- Commands are clamped rather than rejected: a control loop that overshoots
-- should be pinned to the safe end, not made to fail. The clamped value is
-- returned so callers can notice.
-- ---------------------------------------------------------------------------

function mt:set_pos_limits(id, min_pos, max_pos)
    check_id(id)
    min_pos = check_u16(min_pos, "min_pos")
    max_pos = check_u16(max_pos, "max_pos")
    if min_pos > max_pos then
        error(string.format("scs_servo: min_pos %d must not exceed max_pos %d", min_pos, max_pos))
    end
    self._pos_limits[id] = {min_pos, max_pos}
end

function mt:get_pos_limits(id)
    local limits = self._pos_limits[check_id(id)]
    if not limits then
        return nil
    end
    return limits[1], limits[2]
end

function mt:clear_pos_limits(id)
    self._pos_limits[check_id(id)] = nil
end

local function clamp_position(self, id, position)
    local limits = self._pos_limits[id] or self._default_limits
    if not limits then
        return position
    end
    if position < limits[1] then
        return limits[1]
    end
    if position > limits[2] then
        return limits[2]
    end
    return position
end

-- ---------------------------------------------------------------------------
-- Position mode
-- ---------------------------------------------------------------------------

-- `move_time` is the time budget for the move in servo units, `speed` is a
-- speed cap (0 means unlimited). Returns true plus the position actually sent,
-- which differs from `position` when a limit clamped it.
function mt:write_pos(id, position, move_time, speed)
    check_id(id)
    position = clamp_position(self, id, check_u16(position, "position"))
    local pos_h, pos_l = split_u16(position)
    local time_h, time_l = split_u16(check_u16(move_time or 0, "move_time"))
    local speed_h, speed_l = split_u16(check_u16(speed or 0, "speed"))
    local ok, err = self:write_bytes(id, REG.GOAL_POSITION_L,
        {pos_h, pos_l, time_h, time_l, speed_h, speed_l})
    if ok == nil then
        return nil, err
    end
    return true, position
end

-- Queue a position without moving, then release every queued servo at once.
function mt:reg_write_pos(id, position, move_time, speed)
    check_id(id)
    position = clamp_position(self, id, check_u16(position, "position"))
    local pos_h, pos_l = split_u16(position)
    local time_h, time_l = split_u16(check_u16(move_time or 0, "move_time"))
    local speed_h, speed_l = split_u16(check_u16(speed or 0, "speed"))
    local ok, err = transact(self, id, INST_REG_WRITE, REG.GOAL_POSITION_L,
        {pos_h, pos_l, time_h, time_l, speed_h, speed_l}, 0)
    if ok == nil then
        return nil, err
    end
    return true, position
end

function mt:action(id)
    id = id or BROADCAST_ID
    check_id(id)
    local ok, err = transact(self, id, INST_REG_ACTION, 0, nil, 0)
    if ok == nil then
        return nil, err
    end
    return true
end

-- One broadcast frame that moves several servos together. `times` and `speeds`
-- may be omitted; per-servo entries default to 0.
function mt:sync_write_pos(ids, positions, times, speeds)
    if type(ids) ~= "table" or type(positions) ~= "table" then
        error("scs_servo: sync_write_pos needs ids and positions arrays")
    end
    if #ids == 0 then
        error("scs_servo: sync_write_pos ids array is empty")
    end
    if #positions ~= #ids then
        error("scs_servo: sync_write_pos positions length must match ids")
    end

    local per_servo = 6
    local msg_len = (per_servo + 1) * #ids + 4
    local bytes = {BROADCAST_ID, msg_len, INST_SYNC_WRITE, REG.GOAL_POSITION_L, per_servo}
    local sum = BROADCAST_ID + msg_len + INST_SYNC_WRITE + REG.GOAL_POSITION_L + per_servo

    for i = 1, #ids do
        local id = check_id(ids[i])
        local position = clamp_position(self, id, check_u16(positions[i], "position"))
        local pos_h, pos_l = split_u16(position)
        local time_h, time_l = split_u16(check_u16(times and times[i] or 0, "move_time"))
        local speed_h, speed_l = split_u16(check_u16(speeds and speeds[i] or 0, "speed"))
        local entry = {id, pos_h, pos_l, time_h, time_l, speed_h, speed_l}
        for j = 1, #entry do
            bytes[#bytes + 1] = entry[j]
            sum = sum + entry[j]
        end
    end

    bytes[#bytes + 1] = (~sum) & 0xFF
    self._uart:flush_input()
    self._uart:write(HEADER .. string.char(table.unpack(bytes)))
    return true
end

-- ---------------------------------------------------------------------------
-- Feedback
-- ---------------------------------------------------------------------------

function mt:read_pos(id)
    return self:read_word(id, REG.PRESENT_POSITION_L)
end

function mt:read_speed(id)
    local raw, err = self:read_word(id, REG.PRESENT_SPEED_L)
    if raw == nil then
        return nil, err
    end
    return apply_sign(raw, SPEED_SIGN_BIT)
end

function mt:read_load(id)
    local raw, err = self:read_word(id, REG.PRESENT_LOAD_L)
    if raw == nil then
        return nil, err
    end
    return apply_sign(raw, LOAD_SIGN_BIT)
end

function mt:read_current(id)
    local raw, err = self:read_word(id, REG.PRESENT_CURRENT_L)
    if raw == nil then
        return nil, err
    end
    return apply_sign(raw, CURRENT_SIGN_BIT)
end

function mt:read_voltage(id)
    return self:read_byte(id, REG.PRESENT_VOLTAGE)
end

function mt:read_temperature(id)
    return self:read_byte(id, REG.PRESENT_TEMPERATURE)
end

-- Non-zero while the servo is still travelling toward its goal.
function mt:read_moving(id)
    return self:read_byte(id, REG.MOVING)
end

-- One 15-byte burst read covering registers 56..70 (position through current),
-- so a control loop needs a single round trip instead of six.
function mt:read_feedback(id)
    local payload, err = self:read_bytes(id, REG.PRESENT_POSITION_L, 15)
    if payload == nil then
        return nil, err
    end

    -- `offset` is 1-based within the payload; register 56 lands at offset 1.
    local function word(offset)
        return join_u16(string.byte(payload, offset), string.byte(payload, offset + 1))
    end

    return {
        position = word(1),                                     -- 56, 57
        speed = apply_sign(word(3), SPEED_SIGN_BIT),             -- 58, 59
        load = apply_sign(word(5), LOAD_SIGN_BIT),               -- 60, 61
        voltage = string.byte(payload, 7),                       -- 62
        temperature = string.byte(payload, 8),                   -- 63
        moving = string.byte(payload, 11),                       -- 66
        current = apply_sign(word(14), CURRENT_SIGN_BIT),        -- 69, 70
    }
end

-- ---------------------------------------------------------------------------
-- Torque and EPROM lock
-- ---------------------------------------------------------------------------

function mt:enable_torque(id, enabled)
    return self:write_byte(id, REG.TORQUE_ENABLE, enabled and 1 or 0)
end

function mt:read_torque_enable(id)
    -- The reference library reads a word here, which folds the neighbouring
    -- register into the answer; TORQUE_ENABLE is one byte, so read one byte.
    local raw, err = self:read_byte(id, REG.TORQUE_ENABLE)
    if raw == nil then
        return nil, err
    end
    return raw ~= 0
end

function mt:unlock_eprom(id)
    return self:write_byte(id, REG.LOCK, 0)
end

function mt:lock_eprom(id)
    return self:write_byte(id, REG.LOCK, 1)
end

-- ---------------------------------------------------------------------------
-- Angle limits and PWM mode
--
-- The SCSCL has no mode register: PWM mode is entered by zeroing the min/max
-- angle limits, which live in EPROM. Switching modes therefore writes EPROM
-- every time, so avoid doing it in a loop.
-- ---------------------------------------------------------------------------

function mt:read_angle_limits(id)
    local min_angle, err = self:read_word(id, REG.MIN_ANGLE_LIMIT_L)
    if min_angle == nil then
        return nil, err
    end
    local max_angle
    max_angle, err = self:read_word(id, REG.MAX_ANGLE_LIMIT_L)
    if max_angle == nil then
        return nil, err
    end
    return min_angle, max_angle
end

function mt:write_angle_limits(id, min_angle, max_angle)
    local ok, err = self:write_word(id, REG.MIN_ANGLE_LIMIT_L, min_angle)
    if ok == nil then
        return nil, err
    end
    return self:write_word(id, REG.MAX_ANGLE_LIMIT_L, max_angle)
end

-- Enter PWM mode by zeroing both angle limits.
function mt:pwm_mode(id)
    return self:write_bytes(id, REG.MIN_ANGLE_LIMIT_L, {0, 0, 0, 0})
end

-- `mode` is M.MODE_POSITION or M.MODE_PWM. Entering PWM mode caches the current
-- angle limits so returning to position mode can restore them, and returns them
-- as `true, min, max`.
--
-- WARNING: the angle limits live in EPROM, so a servo left in PWM mode has no
-- travel protection at all -- and the zeroed limits survive a reboot. If the
-- script that entered PWM mode dies before switching back, restore them with
-- `write_angle_limits(id, min, max)` using the values returned here. Persist
-- them somewhere if that matters; this cache only lives as long as the handle.
function mt:switch_mode(id, mode)
    check_id(id)
    if mode ~= M.MODE_POSITION and mode ~= M.MODE_PWM then
        error("scs_servo: mode must be M.MODE_POSITION or M.MODE_PWM")
    end

    if mode == M.MODE_PWM then
        local min_angle, max_angle = self:read_angle_limits(id)
        if min_angle == nil then
            return nil, "cannot read angle limits"
        end
        self._angle_limits[id] = {min_angle, max_angle}
        local ok, err = self:pwm_mode(id)
        if ok == nil then
            return nil, err
        end
        return true, min_angle, max_angle
    end

    local cached = self._angle_limits[id]
    if cached == nil then
        return nil, "no cached angle limits; call write_angle_limits explicitly"
    end
    return self:write_angle_limits(id, cached[1], cached[2])
end

-- Continuous rotation drive, -1023..1023. Only meaningful in PWM mode.
function mt:write_pwm(id, pwm)
    if type(pwm) ~= "number" then
        error("scs_servo: pwm must be a number")
    end
    pwm = math.floor(pwm)
    if pwm < -PWM_MAX or pwm > PWM_MAX then
        error(string.format("scs_servo: pwm %d out of range -%d..%d", pwm, PWM_MAX, PWM_MAX))
    end

    local encoded = pwm
    if encoded < 0 then
        encoded = (-encoded) | PWM_SIGN_BIT
    end
    local high, low = split_u16(encoded)
    return self:write_bytes(id, REG.GOAL_TIME_L, {high, low})
end

-- ---------------------------------------------------------------------------
-- Teardown
-- ---------------------------------------------------------------------------

function mt:close()
    if self._owns_uart and self._uart then
        self._uart:close()
    end
    self._uart = nil
end

function mt:__gc()
    pcall(function()
        self:close()
    end)
end

return M
