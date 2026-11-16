-- StackChan head-touch monitor.
--
-- The Si12T 12-channel capacitive touch IC lives on the StackChan Touch board at
-- I2C 0x68 (ID_SEL tied to GND), on the same FPC that carries the ring LEDs and
-- BUS_5V.
--
-- Topology, established on hardware (2026-08-26): there is exactly ONE touch
-- electrode, on the top of the head. The chip has 12 channels but they are not
-- 12 pads around the head, so there is no TS-to-location mapping to discover.
-- A single press reports several channels at once and they chatter against the
-- threshold as the contact patch shifts -- an earlier run of this script logged
-- 78 "press" events from a handful of real touches, cycling TS1/TS2/TS3.
--
-- So this script treats "any channel active" as one logical zone and debounces
-- it, which is the shape the rest of the firmware wants: one press, one release,
-- one hold duration. Raw per-channel output is still available with raw=true for
-- checking the FPC or re-examining which channels sit under the pad.
--
-- Defaults target StackChan, so no --args-json is needed.
--
--   lua --run --path /system/scripts/stackchan_touch.lua
--   lua --run --path /system/scripts/stackchan_touch.lua --args-json "{\"seconds\":60}"
--   lua --run --path /system/scripts/stackchan_touch.lua --args-json "{\"raw\":true}"

local si12t_touch = require("lib_si12t_touch")
local i2c = require("i2c")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local PORT = int_arg("port", 0)
local SDA = int_arg("sda", 12)
local SCL = int_arg("scl", 11)
local ADDR = int_arg("addr", 0x68)
local SECONDS = int_arg("seconds", 30)
local INTERVAL_MS = int_arg("interval_ms", 50)
-- Lower is more sensitive. The library default is 3; raise it if the idle state
-- reports touches, lower it if pressing through the shell is missed.
local THRESHOLD = int_arg("threshold", nil)
-- Debounce is deliberately asymmetric: react fast, let go slowly. Chatter shows
-- up as brief drops to zero mid-press, so the release filter is the long one.
local PRESS_SAMPLES = int_arg("press_samples", 2)
local RELEASE_SAMPLES = int_arg("release_samples", 6)
local RAW = a.raw == true

local CHANNELS = si12t_touch.channels()

local touch
local bus
local owns_bus = false

local function cleanup()
    if touch then
        pcall(function() touch:close() end)
        touch = nil
    end
    if owns_bus and bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

-- "..X..X......" is easier to scan at a glance than a hex mask.
local function bitmap(mask)
    local chars = {}
    for ch = 0, CHANNELS - 1 do
        chars[#chars + 1] = ((mask >> ch) & 1) == 1 and "X" or "."
    end
    return table.concat(chars)
end

local function active_list(mask)
    local names = {}
    for ch = 0, CHANNELS - 1 do
        if ((mask >> ch) & 1) == 1 then
            names[#names + 1] = "TS" .. (ch + 1)
        end
    end
    if #names == 0 then
        return "-"
    end
    return table.concat(names, " ")
end

local function run()
    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(PORT, SDA, SCL, 100000)
        owns_bus = true
    end

    local opts = {bus = bus, addr = ADDR}
    if THRESHOLD then
        opts.threshold = THRESHOLD
    end
    touch = si12t_touch.new(opts)

    print(string.format("[touch] Si12T at 0x%02X, threshold=%d, %d channels",
        touch:address(), touch:threshold(), CHANNELS))
    print(string.format("[touch] one logical zone: top of head (press >=%d samples, release >=%d)",
        PRESS_SAMPLES, RELEASE_SAMPLES))
    print(string.format("[touch] listening for %d s, polling every %d ms",
        SECONDS, INTERVAL_MS))
    if RAW then
        print("[touch] raw mode: every non-idle sample is printed, TS1 leftmost")
    end

    local iterations = math.floor(SECONDS * 1000 / INTERVAL_MS)

    local held = false
    local on_streak = 0        -- consecutive samples with any channel active
    local off_streak = 0       -- consecutive idle samples while still held
    local hold_samples = 0     -- length of the current hold, gaps included
    local hold_billed = 0      -- hold_samples as of the last active sample
    local hold_mask = 0        -- channels that contributed to the current hold
    local touches = 0
    local seen = 0
    local total_hold_samples = 0
    local longest_hold = 0

    local function end_hold(unterminated)
        -- The trailing idle samples only confirmed the release, so the hold is
        -- billed up to the last active sample instead.
        local ms = hold_billed * INTERVAL_MS
        total_hold_samples = total_hold_samples + hold_billed
        if hold_billed > longest_hold then
            longest_hold = hold_billed
        end
        print(string.format("[touch] #%-3d release  %4d ms  %s%s",
            touches, ms, active_list(hold_mask),
            unterminated and "  (still held at end of run)" or ""))
        held = false
        hold_samples = 0
        hold_billed = 0
        hold_mask = 0
        off_streak = 0
    end

    for i = 1, iterations do
        local mask = touch:read()

        if RAW and mask ~= 0 then
            print(string.format("[touch] raw  %s  %s", bitmap(mask), active_list(mask)))
        end

        if mask ~= 0 then
            seen = seen | mask
            off_streak = 0
            on_streak = on_streak + 1
            if held then
                hold_mask = hold_mask | mask
                hold_samples = hold_samples + 1
            elseif on_streak >= PRESS_SAMPLES then
                held = true
                touches = touches + 1
                hold_mask = mask
                -- The samples that satisfied the filter were part of the press.
                hold_samples = on_streak
                print(string.format("[touch] #%-3d press", touches))
            end
            if held then
                hold_billed = hold_samples
            end
        else
            on_streak = 0
            if held then
                off_streak = off_streak + 1
                if off_streak >= RELEASE_SAMPLES then
                    end_hold(false)
                else
                    -- A brief drop inside a press is chatter, not a release.
                    hold_samples = hold_samples + 1
                end
            end
        end

        if i < iterations then
            delay.delay_ms(INTERVAL_MS)
        end
    end

    if held then
        end_hold(true)
    end

    print("[touch] --- results ---")
    print(string.format("[touch] %d touch%s in %d s", touches,
        touches == 1 and "" or "es", SECONDS))
    if touches > 0 then
        print(string.format("[touch] hold: longest %d ms, average %d ms",
            longest_hold * INTERVAL_MS,
            math.floor(total_hold_samples * INTERVAL_MS / touches)))
        print(string.format("[touch] channels under the pad: %s", active_list(seen)))
        local unseen = (~seen) & ((1 << CHANNELS) - 1)
        if unseen ~= 0 then
            print(string.format("[touch] silent: %s (expected -- one electrode, 12 channels)",
                active_list(unseen)))
        end
    else
        print("[touch] Nothing fired at all. Check that the Touch board FPC is seated;")
        print("[touch]   the ring LEDs share that connector, so try py32_ioe_basic.lua too.")
        print("[touch]   If the chip answers but presses are missed, lower the threshold.")
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[touch] ERROR: " .. (tostring(err):gsub("\n.*", "")))
    error(err)
end
