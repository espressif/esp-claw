-- --------------------------------------------------------------
-- StackChan head motion: semantic moves, gestures, and per-unit
-- zero-point calibration on top of lib_scs_servo.
--
-- Angles are degrees measured from the stored zero. Counts are the
-- servo's raw encoder units (1 count = 0.3125 deg). Everything the
-- model asks for is expressed in degrees; counts only appear in
-- calibration and jog output.
--
-- Zero points and travel limits are per-unit mechanical properties,
-- so they live in a JSON file under the DATA root rather than in
-- this script. The defaults below are the values measured on one
-- unit on 2026-08-26 and are only a starting point.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local scs = require("lib_scs_servo")
local storage = require("storage")
local json = require("json")
local delay = require("delay")

-- 2. Constants
local UART_PORT = 1
local UART_TX = 6
local UART_RX = 7
local UART_BAUD = 1000000

local COUNTS_PER_DEG = 3.2              -- 1 count = 0.3125 deg
local CONFIG_REL_PATH = "config/stackchan_head.json"
local CONFIG_VERSION = 1

-- Per-axis geometry. Three different bounds, which are easy to conflate:
--
--   encoder_*  what position mode can address at all (0..1023 on this servo).
--   envelope_* the outer bound any configuration is allowed to name. For pitch
--              this is a little wider than any plausible unit, so one unit's
--              calibration is never rejected, while a corrupt file still cannot
--              ask for the full encoder range. Yaw has no stop, so it is the
--              encoder range.
--   def_*      factory defaults, used until this unit has been calibrated.
--
-- `stops` says whether a hand sweep can discover the safe travel at all. Pitch
-- has real mechanical stops, so it can. Yaw turns further than the encoder can
-- express and its real limit is cable twist through the joint, so it cannot.
--
-- Measured: pitch stops at 576..862 on one unit and 581..863 on another, both
-- close to the 90 deg design figure. Positive angles are up (pitch) and left
-- (yaw), confirmed on hardware 2026-08-28.
local AXES = {
    yaw = {
        id = 1,
        encoder_min = 0, encoder_max = 1023,
        envelope_min = 0, envelope_max = 1023,
        -- Encoder midpoint. The mechanical centre is not observable from a
        -- sweep, so a real zero needs calibrate_zero.
        def_zero = 512, def_min = 224, def_max = 800,
        min_span = 64, max_span = 1023,
        stops = false,
    },
    pitch = {
        id = 2,
        encoder_min = 0, encoder_max = 1023,
        envelope_min = 560, envelope_max = 880,
        -- Midpoint of the measured travel, i.e. roughly level.
        def_zero = 719, def_min = 584, def_max = 854,
        -- 90 deg is 288 counts; accept a measured travel of 75..100 deg.
        min_span = 240, max_span = 320,
        stops = true,
    },
}
local AXIS_ORDER = {"yaw", "pitch"}

local MAX_DURATION_MS = 3000
local JOG_MAX_COUNTS = 40
local GESTURE_MAX_TIMES = 10
local CAL_SAMPLE_INTERVAL_MS = 100
local CAL_MARGIN_COUNTS = 8
-- A move reports where the head ended up, so it has to wait for the servo to
-- stop rather than for the move-time budget to elapse.
local SETTLE_POLL_MS = 40
local SETTLE_MAX_MS = 800
local SETTLE_TOLERANCE_COUNTS = 8

-- 3. Args
local ACTIONS = {
    status = true, move = true, center = true, nod = true, shake = true,
    torque = true, jog = true, calibrate_zero = true, calibrate_range = true,
    set_direction = true,
}

local ctx = arg_schema.parse(args, {
    duration_ms = arg_schema.int({default = 600, min = 0, max = MAX_DURATION_MS}),
    times = arg_schema.int({default = 2, min = 1, max = GESTURE_MAX_TIMES}),
    amplitude_deg = arg_schema.int({default = 12, min = 1, max = 60}),
    counts = arg_schema.int({default = 0, min = -JOG_MAX_COUNTS, max = JOG_MAX_COUNTS}),
    seconds = arg_schema.int({default = 15, min = 1, max = 120}),
    direction = arg_schema.int({default = 1, min = -1, max = 1}),
    relative = arg_schema.bool({default = false}),
    enabled = arg_schema.bool({default = true}),
    mid_travel = arg_schema.bool({default = false}),
    -- On by default: yaw's cable drag leaves it about 4 deg short of any goal,
    -- measured on hardware, and one correction pass lands it exactly. Pass
    -- trim=false when a move should be as quick as possible.
    trim = arg_schema.bool({default = true}),
})

local raw_args = type(args) == "table" and args or {}

local function want_string(key, allowed, default)
    local v = raw_args[key]
    if v == nil then
        return default
    end
    if type(v) ~= "string" or not allowed[v] then
        error(string.format("%s must be one of the documented values, got %s",
            key, tostring(v)))
    end
    return v
end

-- Angles are optional: an omitted axis holds its current position.
local function want_deg(key)
    local v = raw_args[key]
    if v == nil then
        return nil
    end
    if type(v) ~= "number" then
        error(key .. " must be a number of degrees")
    end
    return v
end

local ACTION = want_string("action", ACTIONS, "status")
local AXIS_NAMES = {yaw = true, pitch = true, both = true}
local AXIS_ARG = want_string("axis", AXIS_NAMES, "both")
local YAW_DEG = want_deg("yaw_deg")
local PITCH_DEG = want_deg("pitch_deg")

-- 4. Configuration: zero points and travel limits live under DATA.
local function config_path()
    return storage.join_path(storage.get_root_dir(), CONFIG_REL_PATH)
end

local function default_axis(ax)
    return {zero = ax.def_zero, dir = 1, count_min = ax.def_min, count_max = ax.def_max}
end

local function default_config()
    local cfg = {version = CONFIG_VERSION, calibrated = false}
    for name, ax in pairs(AXES) do
        cfg[name] = default_axis(ax)
    end
    return cfg
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- A configuration file may name any travel inside the axis envelope, because a
-- calibrated unit legitimately differs from the factory defaults. What it may
-- not do is name a travel that no real unit could have: that is a damaged or
-- hand-edited file, and falling back to the known-good defaults is safer than
-- clamping whatever it said. A zero outside the travel would make most angles
-- unreachable, so that is pulled in.
local function sanitize_axis(name, axis)
    local ax = AXES[name]
    local out = default_axis(ax)
    if type(axis) ~= "table" then
        return out, {name .. ": missing, using defaults"}
    end

    local notes = {}
    local lo = type(axis.count_min) == "number" and math.floor(axis.count_min) or nil
    local hi = type(axis.count_max) == "number" and math.floor(axis.count_max) or nil

    if lo and hi and lo < hi then
        local span = hi - lo
        if span < ax.min_span or span > ax.max_span then
            notes[#notes + 1] = string.format(
                "%s: travel span %d counts is not plausible for this axis, using defaults",
                name, span)
        else
            out.count_min = clamp(lo, ax.envelope_min, ax.envelope_max)
            out.count_max = clamp(hi, ax.envelope_min, ax.envelope_max)
            if out.count_min ~= lo or out.count_max ~= hi then
                notes[#notes + 1] = string.format(
                    "%s: travel pulled inside the envelope %d..%d",
                    name, ax.envelope_min, ax.envelope_max)
            end
        end
    else
        notes[#notes + 1] = name .. ": travel invalid, using defaults"
    end

    if axis.dir == -1 then
        out.dir = -1
    elseif axis.dir ~= 1 and axis.dir ~= nil then
        notes[#notes + 1] = name .. ": direction invalid, using +1"
    end

    if type(axis.zero) == "number" then
        out.zero = clamp(math.floor(axis.zero), out.count_min, out.count_max)
        if out.zero ~= math.floor(axis.zero) then
            notes[#notes + 1] = name .. ": zero was outside the travel, clamped"
        end
    else
        out.zero = clamp(out.zero, out.count_min, out.count_max)
        notes[#notes + 1] = name .. ": zero missing, using default"
    end

    return out, notes
end

local function load_config()
    local path = config_path()
    if not storage.exists(path) then
        return default_config(), "defaults (no calibration file yet)"
    end

    local ok, decoded = pcall(function()
        return json.decode(storage.read_file(path))
    end)
    if not ok or type(decoded) ~= "table" then
        return default_config(), "defaults (calibration file unreadable)"
    end

    local cfg = {version = CONFIG_VERSION, calibrated = decoded.calibrated == true}
    local notes = {}
    for name in pairs(AXES) do
        local axis, axis_notes = sanitize_axis(name, decoded[name])
        cfg[name] = axis
        for _, n in ipairs(axis_notes) do
            notes[#notes + 1] = n
        end
    end

    local source = path
    if #notes > 0 then
        source = path .. " (repaired: " .. table.concat(notes, "; ") .. ")"
    end
    return cfg, source
end

local function save_config(cfg)
    cfg.version = CONFIG_VERSION
    local path = config_path()
    local dir = storage.join_path(storage.get_root_dir(), "config")
    if not storage.exists(dir) then
        storage.mkdir(dir)
    end
    storage.write_file(path, json.encode(cfg))
    print("[head] saved " .. path)
end

-- 5. Angle <-> counts, always through the axis configuration.
local function to_counts(axis, deg)
    return math.floor(axis.zero + axis.dir * deg * COUNTS_PER_DEG + 0.5)
end

local function to_deg(axis, counts)
    return (counts - axis.zero) * axis.dir / COUNTS_PER_DEG
end

local function deg_limits(axis)
    local a = to_deg(axis, axis.count_min)
    local b = to_deg(axis, axis.count_max)
    if a > b then
        return b, a
    end
    return a, b
end

-- Returns the counts actually usable plus the requested degrees after clamping,
-- so callers can tell the user their angle was out of range.
local function counts_for(name, axis, deg)
    local counts = to_counts(axis, deg)
    local limited = clamp(counts, axis.count_min, axis.count_max)
    if limited ~= counts then
        local lo, hi = deg_limits(axis)
        print(string.format(
            "[head] %s %.1f deg is outside %.1f..%.1f deg, clamped to %.1f deg",
            name, deg, lo, hi, to_deg(axis, limited)))
    end
    return limited, to_deg(axis, limited)
end

-- 6. Bus
local cfg, cfg_source = load_config()
local bus
-- Which axes answered their ping. A silent servo disables only the actions that
-- need it, so a broken yaw cable does not stop the head nodding.
local alive = {}

-- Called by every action that moves or reads a specific axis.
local function require_axes(names)
    local missing = {}
    for _, name in ipairs(names) do
        if not alive[name] then
            missing[#missing + 1] = string.format("%s (id %d)", name, AXES[name].id)
        end
    end
    if #missing > 0 then
        error(string.format("%s did not answer, so this action cannot run",
            table.concat(missing, ", ")))
    end
end

local function cleanup()
    if bus then
        local ok, err = pcall(function() bus:close() end)
        if not ok then
            print("[head] WARN: bus close failed: " .. tostring(err))
        end
        bus = nil
    end
end

local function open_bus()
    -- Hand the travel limits to the driver as well, so even a raw jog cannot
    -- drive past them.
    local pos_limits = {}
    for name, ax in pairs(AXES) do
        pos_limits[ax.id] = {cfg[name].count_min, cfg[name].count_max}
    end
    bus = scs.new({
        port = UART_PORT, tx = UART_TX, rx = UART_RX, baud = UART_BAUD,
        pos_limits = pos_limits,
    })
end

-- This bus drops a frame now and then: a servo resetting mid-reply, or bytes
-- left behind by a read that timed out. Every operation here is idempotent --
-- the same register read, the same goal, the same torque state -- and the driver
-- flushes RX before each request, so one retry separates a transient frame error
-- from a servo that is really gone. Retries are printed rather than swallowed,
-- so an intermittent bus stays visible.
local BUS_ATTEMPTS = 2

local function bus_try(what, fn)
    local value, err
    for attempt = 1, BUS_ATTEMPTS do
        value, err = fn()
        if value ~= nil then
            if attempt > 1 then
                print(string.format("[head] note: %s needed %d attempts", what, attempt))
            end
            return value
        end
    end
    error(string.format("%s failed: %s", what, tostring(err)))
end

-- A half-duplex bus occasionally hands back one unusable frame: a servo that
-- reset mid-reply, or bytes left over from a read that timed out. The driver
-- flushes RX before every request, so a single retry separates a transient
-- framing error from a servo that is really gone -- and aborting the whole
-- action on the first garbled byte would be worse than either.
local function read_counts(name)
    local id = AXES[name].id
    return bus_try("read " .. name .. " position", function()
        return bus:read_pos(id)
    end)
end

local function selected_axes()
    if AXIS_ARG == "both" then
        return AXIS_ORDER
    end
    return {AXIS_ARG}
end

-- The servo stops driving as soon as it is inside its own dead band, so that is
-- the floor on how precisely any goal can be met. Read once per run.
local dead_band = {}

local function dead_band_counts(name)
    if dead_band[name] == nil then
        local id = AXES[name].id
        local cw = bus:read_byte(id, scs.REG.CW_DEAD) or 0
        local ccw = bus:read_byte(id, scs.REG.CCW_DEAD) or 0
        dead_band[name] = math.max(cw, ccw)
    end
    return dead_band[name]
end

-- Anything inside the dead band (plus a count of slack) is the servo working as
-- configured, not a fault worth reporting.
local function tolerance_counts(name)
    local band = dead_band_counts(name) + 2
    if band > SETTLE_TOLERANCE_COUNTS then
        return band
    end
    return SETTLE_TOLERANCE_COUNTS
end

local function set_torque(enabled)
    for _, name in ipairs(selected_axes()) do
        local id = AXES[name].id
        bus_try((enabled and "enable" or "disable") .. " " .. name .. " torque",
            function() return bus:enable_torque(id, enabled) end)
    end
end

-- `move_time` is a budget, not a promise: waiting exactly that long reports a
-- position the head is still travelling through. Wait for MOVING to clear.
local function wait_settled(ids, duration)
    delay.delay_ms(duration)
    local waited = 0
    while waited < SETTLE_MAX_MS do
        local moving = false
        for _, id in ipairs(ids) do
            local value = bus:read_moving(id)
            if value and value ~= 0 then
                moving = true
            end
        end
        if not moving then
            return
        end
        delay.delay_ms(SETTLE_POLL_MS)
        waited = waited + SETTLE_POLL_MS
    end
end

-- Drive both axes in one bus transaction when both are requested, so the head
-- moves as one instead of stepping yaw then pitch.
local function send(targets, duration)
    local ids, positions, times, speeds = {}, {}, {}, {}
    for _, name in ipairs(AXIS_ORDER) do
        local counts = targets[name]
        if counts then
            local ax = AXES[name]
            bus_try("enable " .. name .. " torque",
                function() return bus:enable_torque(ax.id, true) end)
            ids[#ids + 1] = ax.id
            positions[#positions + 1] = counts
            times[#times + 1] = duration
            speeds[#speeds + 1] = 0
        end
    end
    if #ids == 0 then
        return
    end
    if #ids == 1 then
        bus_try("write position", function()
            return bus:write_pos(ids[1], positions[1], duration, 0)
        end)
    else
        -- A broadcast frame is never acknowledged, so there is nothing to retry.
        local ok, err = bus:sync_write_pos(ids, positions, times, speeds)
        if ok == nil then
            error("sync write failed: " .. tostring(err))
        end
    end
    wait_settled(ids, duration)
end

-- With `trim`, one correction pass: an axis that settled outside its dead band
-- is aimed past the goal by the residual, which pulls it the rest of the way.
-- Exactly one extra move, so this cannot oscillate.
local function go(targets, duration, trim)
    send(targets, duration)
    if not trim then
        return
    end

    local corrections, wanted = {}, false
    for _, name in ipairs(AXIS_ORDER) do
        local target = targets[name]
        if target then
            local residual = target - read_counts(name)
            if math.abs(residual) > tolerance_counts(name) then
                local axis = cfg[name]
                corrections[name] = clamp(target + residual, axis.count_min, axis.count_max)
                wanted = true
                print(string.format("[head] trimming %s by %d counts (%.1f deg)",
                    name, residual, residual / COUNTS_PER_DEG))
            end
        end
    end
    if wanted then
        send(corrections, duration)
    end
end

-- Reports where the head actually is, and flags an axis that stopped short:
-- that means something is in the way, or the servo is loaded past what it can
-- hold, not that the angle was wrong.
local function report_pose(prefix, targets, trimmed)
    for _, name in ipairs(AXIS_ORDER) do
      if alive[name] then
        local counts = read_counts(name)
        print(string.format("[head] %s %s %.1f deg (counts %d)",
            prefix, name, to_deg(cfg[name], counts), counts))

        local target = targets and targets[name]
        if target and math.abs(counts - target) > tolerance_counts(name) then
            print(string.format(
                "[head]   %s stopped %.1f deg short of %.1f deg (counts %d vs %d,"
                .. " dead band %d): something may be blocking the head, the servo"
                .. " cannot hold that pose, or cable drag is pulling it back%s",
                name, math.abs(counts - target) / COUNTS_PER_DEG,
                to_deg(cfg[name], target), counts, target, dead_band_counts(name),
                trimmed and " -- and one trim pass did not fix it"
                    or " -- pass trim=true to correct it"))
        end
      end
    end
end

-- 7. Actions
local actions = {}

function actions.status()
    print("[head] calibration source: " .. cfg_source)
    print("[head] calibrated: " .. tostring(cfg.calibrated))
    for _, name in ipairs(AXIS_ORDER) do
      local axis = cfg[name]
      if not alive[name] then
        local lo, hi = deg_limits(axis)
        print(string.format(
            "[head] %-5s SILENT   allowed %.1f..%.1f deg   counts %d..%d   zero %d   dir %+d",
            name, lo, hi, axis.count_min, axis.count_max, axis.zero, axis.dir))
      else
        local counts = read_counts(name)
        local lo, hi = deg_limits(axis)
        print(string.format(
            "[head] %-5s %6.1f deg   allowed %.1f..%.1f deg   counts %d (%d..%d)   zero %d   dir %+d",
            name, to_deg(axis, counts), lo, hi, counts,
            axis.count_min, axis.count_max, axis.zero, axis.dir))
        -- The dead band bounds how precisely any goal can be met, and the load
        -- says whether the servo is fighting something to hold this pose.
        local band = dead_band_counts(name)
        local load = bus:read_load(AXES[name].id)
        print(string.format("[head]   dead band %d counts (%.1f deg), load %s",
            band, band / COUNTS_PER_DEG, load and tostring(load) or "?"))
      end
    end
    -- A brown-out or a stalled servo looks like a mechanical fault otherwise.
    local volts = bus:read_voltage(AXES.pitch.id)
    local temp = bus:read_temperature(AXES.pitch.id)
    if volts and temp then
        print(string.format("[head] servo bus %.1f V, pitch servo %d C", volts / 10, temp))
    end
    if not cfg.calibrated then
        print("[head] NOTE: zero points are factory guesses; run calibrate_zero"
            .. " with the head physically centred before trusting angles.")
    end
end

function actions.move()
    if YAW_DEG == nil and PITCH_DEG == nil then
        error("move needs yaw_deg and/or pitch_deg")
    end

    local wanted = {yaw = YAW_DEG, pitch = PITCH_DEG}
    local needed = {}
    for _, name in ipairs(AXIS_ORDER) do
        if wanted[name] then
            needed[#needed + 1] = name
        end
    end
    require_axes(needed)

    local targets = {}
    for _, name in ipairs(AXIS_ORDER) do
        local deg = wanted[name]
        if deg then
            if ctx.relative then
                deg = to_deg(cfg[name], read_counts(name)) + deg
            end
            local counts, actual = counts_for(name, cfg[name], deg)
            targets[name] = counts
            print(string.format("[head] %s -> %.1f deg (counts %d)", name, actual, counts))
        end
    end

    go(targets, ctx.duration_ms, ctx.trim)
    report_pose("now", targets, ctx.trim)
end

function actions.center()
    local targets = {}
    local any = false
    for _, name in ipairs(AXIS_ORDER) do
        if alive[name] then
            targets[name] = select(1, counts_for(name, cfg[name], 0))
            any = true
        end
    end
    if not any then
        error("no axis answered, so there is nothing to centre")
    end
    print("[head] centring on the stored zero")
    go(targets, ctx.duration_ms, ctx.trim)
    report_pose("now", targets, ctx.trim)
end

-- Gestures oscillate around wherever the head already is, so they read as a
-- reaction rather than a move to a fixed pose.
local function gesture(name, label)
    require_axes({name})
    local axis = cfg[name]
    local base = to_deg(axis, read_counts(name))
    local amp = ctx.amplitude_deg
    print(string.format("[head] %s %d times, %d deg around %.1f deg",
        label, ctx.times, amp, base))

    for _ = 1, ctx.times do
        go({[name] = select(1, counts_for(name, axis, base + amp))}, ctx.duration_ms)
        go({[name] = select(1, counts_for(name, axis, base - amp))}, ctx.duration_ms)
    end
    local home = {[name] = select(1, counts_for(name, axis, base))}
    go(home, ctx.duration_ms)
    report_pose("now", home)
end

function actions.nod()
    gesture("pitch", "nodding")
end

function actions.shake()
    gesture("yaw", "shaking")
end

function actions.torque()
    require_axes(selected_axes())
    set_torque(ctx.enabled)
    print(string.format("[head] torque %s on %s",
        ctx.enabled and "enabled" or "released", AXIS_ARG))
    if not ctx.enabled then
        print("[head] the head can now be moved by hand")
    end
end

function actions.jog()
    if AXIS_ARG == "both" then
        error("jog needs axis to be yaw or pitch")
    end
    if ctx.counts == 0 then
        error("jog needs a non-zero counts value")
    end

    local name = AXIS_ARG
    require_axes({name})
    local axis = cfg[name]
    local from = read_counts(name)
    local to = clamp(from + ctx.counts, axis.count_min, axis.count_max)
    print(string.format("[head] jog %s %d counts: %d -> %d (%.1f -> %.1f deg)",
        name, ctx.counts, from, to, to_deg(axis, from), to_deg(axis, to)))
    go({[name] = to}, ctx.duration_ms, ctx.trim)
    report_pose("now", {[name] = to}, ctx.trim)
end

function actions.calibrate_zero()
    if ctx.mid_travel then
        print("[head] taking the midpoint of each calibrated travel as zero")
    else
        print("[head] taking the current pose as zero -- the head must already be"
            .. " where you want 0 deg to be (run action=torque enabled=false first"
            .. " if you want to place it by hand, or pass mid_travel=true to use"
            .. " the middle of the measured travel instead)")
    end

    require_axes(ctx.mid_travel and {} or selected_axes())
    for _, name in ipairs(selected_axes()) do
        local axis = cfg[name]
        local zero
        if ctx.mid_travel then
            zero = (axis.count_min + axis.count_max) // 2
        else
            local counts = read_counts(name)
            zero = clamp(counts, axis.count_min, axis.count_max)
            if zero ~= counts then
                -- Almost always means the head was resting against a stop rather
                -- than being held level: rest is wherever gravity left it, and
                -- the travel keeps a margin off each stop.
                print(string.format(
                    "[head] %s is at %d, outside its travel %d..%d -- it looks like"
                    .. " the head is resting on a stop rather than being held where"
                    .. " you want 0 deg. Zero set to %d, which puts the whole range"
                    .. " on one side of it; hold the axis mid-travel, or pass"
                    .. " mid_travel=true, and run this again.",
                    name, counts, axis.count_min, axis.count_max, zero))
            end
        end

        axis.zero = zero
        local lo, hi = deg_limits(axis)
        print(string.format("[head] %s zero = %d counts, now allows %.1f..%.1f deg",
            name, zero, lo, hi))
        if lo > -1 or hi < 1 then
            print(string.format(
                "[head]   note: %s can now only turn one way from zero", name))
        end
    end
    cfg.calibrated = true
    save_config(cfg)
end

function actions.calibrate_range()
    local names = selected_axes()
    require_axes(names)
    print(string.format(
        "[head] releasing torque; sweep %s by hand for %d s, stopping at each end",
        AXIS_ARG, ctx.seconds))
    set_torque(false)

    local seen = {}
    for _, name in ipairs(names) do
        local counts = read_counts(name)
        seen[name] = {min = counts, max = counts}
    end

    local samples = math.floor(ctx.seconds * 1000 / CAL_SAMPLE_INTERVAL_MS)
    for i = 1, samples do
        for _, name in ipairs(names) do
            local counts = read_counts(name)
            local s = seen[name]
            if counts < s.min then s.min = counts end
            if counts > s.max then s.max = counts end
        end
        if i < samples then
            delay.delay_ms(CAL_SAMPLE_INTERVAL_MS)
        end
    end

    for _, name in ipairs(names) do
        local ax = AXES[name]
        local axis = cfg[name]
        local s = seen[name]
        local span = s.max - s.min
        print(string.format("[head] %s swept %d..%d counts (%.1f deg)",
            name, s.min, s.max, span / COUNTS_PER_DEG))

        if s.min <= ax.encoder_min and s.max >= ax.encoder_max then
            print(string.format("[head] %s reached both encoder ends (%d and %d),"
                .. " so the sweep measured the encoder, not the mechanics",
                name, ax.encoder_min, ax.encoder_max))
        end

        if not ax.stops then
            -- Yaw. The axis turns further than position mode can express, so a
            -- sweep can only ever find the encoder ends. Its real limit is the
            -- servo cable and the Touch board FPC twisting through the joint,
            -- which nothing here can measure -- and widening the travel to
            -- whatever was swept would be strictly worse than the conservative
            -- value already in place.
            print(string.format(
                "[head] %s has no mechanical stop, so a sweep cannot establish its"
                .. " safe travel; keeping %d..%d. Its real limit is cable twist"
                .. " through the joint -- change it deliberately, not from a sweep.",
                name, axis.count_min, axis.count_max))
        elseif span < 4 * CAL_MARGIN_COUNTS then
            print(string.format(
                "[head] %s barely moved; keeping its previous travel %d..%d",
                name, axis.count_min, axis.count_max))
        else
            -- Back off each end so goal positions never sit on a stop.
            axis.count_min = clamp(s.min + CAL_MARGIN_COUNTS, ax.envelope_min, ax.envelope_max)
            axis.count_max = clamp(s.max - CAL_MARGIN_COUNTS, ax.envelope_min, ax.envelope_max)
            axis.zero = clamp(axis.zero, axis.count_min, axis.count_max)
            local lo, hi = deg_limits(axis)
            print(string.format(
                "[head] %s travel = %d..%d counts, allows %.1f..%.1f deg from zero %d",
                name, axis.count_min, axis.count_max, lo, hi, axis.zero))
        end
    end

    save_config(cfg)
    print("[head] run action=calibrate_zero next: a travel range alone does not"
        .. " give a zero point")
end

function actions.set_direction()
    if ctx.direction ~= 1 and ctx.direction ~= -1 then
        error("set_direction needs direction = 1 or -1")
    end
    for _, name in ipairs(selected_axes()) do
        cfg[name].dir = ctx.direction
        local lo, hi = deg_limits(cfg[name])
        print(string.format("[head] %s dir %+d, now allows %.1f..%.1f deg",
            name, ctx.direction, lo, hi))
    end
    save_config(cfg)
end

-- 8. Run
-- Ping both servos before doing anything. One dead axis must not take the other
-- one with it: a silent servo is reported here, and only an action that actually
-- needs it fails.
local function ping_all()
    local found, missing = {}, {}
    for _, name in ipairs(AXIS_ORDER) do
        local ax = AXES[name]
        local answered, attempts = false, 0
        for attempt = 1, BUS_ATTEMPTS do
            attempts = attempt
            if bus:ping(ax.id) then
                answered = true
                break
            end
        end
        if answered then
            alive[name] = true
            found[#found + 1] = name
            if attempts > 1 then
                print(string.format("[head] note: %s answered on ping attempt %d",
                    name, attempts))
            end
        else
            missing[#missing + 1] = string.format("%s (id %d)", name, ax.id)
        end
    end

    if #missing == 0 then
        return
    end
    if #found == 0 then
        error(string.format(
            "no servo answered (%s). That is the servo rail or the bus, not the"
            .. " angle: VM_EN on IOE1 pin 0 feeds the servos, so check it with"
            .. " py32_ioe_basic.lua and the 5 V rail with stackchan_5v_check.lua."
            .. " Also make sure no other script is holding UART %d.",
            table.concat(missing, ", "), UART_PORT))
    end
    print(string.format(
        "[head] WARNING: %s did not answer but %s did, so the bus and the rail are"
        .. " up: suspect that servo's own cable, its id, or an overload shutdown."
        .. " Actions that do not need it still work.",
        table.concat(missing, ", "), table.concat(found, ", ")))
end


local function run()
    open_bus()
    ping_all()
    actions[ACTION]()
end

-- 9. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[head] ERROR: " .. tostring(err))
    error(err)
end
