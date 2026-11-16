-- StackChan servo travel measurement.
--
-- Travel limits are a property of this particular machine, not of the servo:
-- they are "zero point plus mechanical range", and the zero point differs
-- between units (this one rests at yaw=484 / pitch=683 against the reference
-- BSP's 460 / 620). Guessing them risks driving the servo into a hard stop,
-- where it stalls, draws heavy current and heats up. So measure instead.
--
-- This script never drives the servos. It switches torque OFF so the head moves
-- freely by hand, then samples the position encoders and reports the extremes
-- it saw. Feed the result into lib_scs_servo's set_pos_limits().
--
-- Procedure:
--   1. Start the script.
--   2. While it samples, slowly move the head by hand to each mechanical
--      extreme -- left, right, up, down -- and pause briefly at each end.
--   3. Read the suggested limits from the summary.
--
-- Torque is left OFF at the end so nothing snaps back.
--
--   lua --run --path /system/scripts/stackchan_servo_range.lua
--   lua --run --path /system/scripts/stackchan_servo_range.lua --args-json "{\"seconds\":40}"

local scs_servo = require("lib_scs_servo")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local PORT = int_arg("port", 1)
local TX = int_arg("tx", 6)
local RX = int_arg("rx", 7)
local BAUD = int_arg("baud", 1000000)
local SECONDS = int_arg("seconds", 25)
local INTERVAL_MS = int_arg("interval_ms", 100)
-- Shrink the measured span by this many counts at each end, so a limit set from
-- a hand-measured extreme does not sit exactly on the stop.
local MARGIN = int_arg("margin", 8)
local DEG_PER_COUNT = 0.3125
-- SCSCL encoder range. An observed extreme at either boundary means the encoder
-- saturated, which is not the same thing as finding a mechanical stop.
local POS_MAX = int_arg("pos_max", 1023)

local AXES = {
    {id = int_arg("yaw_id", 1), name = "yaw  "},
    {id = int_arg("pitch_id", 2), name = "pitch"},
}

local bus

local function cleanup()
    if bus then
        -- Leave torque off: the head should stay free, not snap to a goal.
        for _, axis in ipairs(AXES) do
            pcall(function() bus:enable_torque(axis.id, false) end)
        end
        pcall(function() bus:close() end)
        bus = nil
    end
end

local function run()
    bus = scs_servo.new({port = PORT, tx = TX, rx = RX, baud = BAUD})

    local tracked = {}
    for _, axis in ipairs(AXES) do
        if bus:ping(axis.id) then
            bus:enable_torque(axis.id, false)
            local pos = bus:read_pos(axis.id)
            tracked[#tracked + 1] = {
                id = axis.id,
                name = axis.name,
                min = pos,
                max = pos,
                samples = 0,
            }
            print(string.format("[range] %s (id %d) torque OFF, resting at %s",
                axis.name, axis.id, tostring(pos)))
        else
            print(string.format("[range] %s (id %d) did not respond, skipping", axis.name, axis.id))
        end
    end

    if #tracked == 0 then
        error("no servos responded; check the VM_EN rail and wiring")
    end

    local iterations = math.floor(SECONDS * 1000 / INTERVAL_MS)
    print(string.format(
        "[range] sampling for %d s -- move the head slowly to every mechanical extreme now",
        SECONDS))

    for i = 1, iterations do
        local changed = false
        for _, axis in ipairs(tracked) do
            local pos = bus:read_pos(axis.id)
            if pos then
                axis.samples = axis.samples + 1
                if pos < axis.min then
                    axis.min = pos
                    changed = true
                end
                if pos > axis.max then
                    axis.max = pos
                    changed = true
                end
            end
        end

        -- Only print when an extreme moved, so the log stays readable and the
        -- user gets live feedback that hand movement is being picked up.
        if changed then
            local parts = {}
            for _, axis in ipairs(tracked) do
                parts[#parts + 1] = string.format("%s %d..%d", axis.name, axis.min, axis.max)
            end
            print(string.format("[range] %s", table.concat(parts, "   ")))
        end

        if i < iterations then
            delay.delay_ms(INTERVAL_MS)
        end
    end

    print("[range] --- results ---")
    for _, axis in ipairs(tracked) do
        local span = axis.max - axis.min
        -- An extreme sitting on the encoder boundary is saturation, not a
        -- mechanical stop: the axis may well turn further than position mode can
        -- address. Trimming a margin off such an end only throws away reachable
        -- travel, so only apply the margin where a real stop was found.
        local min_saturated = axis.min <= 0
        local max_saturated = axis.max >= POS_MAX
        local safe_min = min_saturated and axis.min or (axis.min + MARGIN)
        local safe_max = max_saturated and axis.max or (axis.max - MARGIN)
        if safe_min > safe_max then
            safe_min, safe_max = axis.min, axis.max
        end

        print(string.format(
            "[range] %s (id %d): observed %d..%d  span %d counts (%.1f deg) from %d samples",
            axis.name, axis.id, axis.min, axis.max, span, span * DEG_PER_COUNT, axis.samples))

        if min_saturated or max_saturated then
            print(string.format(
                "[range]   NOTE: %s end%s hit the encoder boundary (0..%d), not a mechanical stop.",
                (min_saturated and max_saturated) and "both" or (min_saturated and "lower" or "upper"),
                (min_saturated and max_saturated) and "s" or "", POS_MAX))
            print("[range]   That axis may rotate further than position mode can address; use PWM")
            print("[range]   mode for continuous rotation. No margin applied at a saturated end.")
        end

        if min_saturated and max_saturated then
            print(string.format(
                "[range]   suggested: no limit needed for id %d -- the default 0..%d fallback",
                axis.id, POS_MAX))
            print("[range]   already covers the whole addressable range.")
        else
            print(string.format(
                "[range]   suggested: bus:set_pos_limits(%d, %d, %d)",
                axis.id, safe_min, safe_max))
        end

        if span < 20 then
            print("[range]   WARNING: span is tiny -- was this axis actually moved by hand?")
        end
    end
    print("[range] These numbers describe THIS unit as assembled right now. Re-measure after")
    print("[range] re-assembly or a servo swap; do not copy them to another machine.")
    print("[range] torque stays OFF; re-enable with enable_torque(id, true) when ready")
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[range] ERROR: " .. (tostring(err):gsub("\n.*", "")))
    error(err)
end
