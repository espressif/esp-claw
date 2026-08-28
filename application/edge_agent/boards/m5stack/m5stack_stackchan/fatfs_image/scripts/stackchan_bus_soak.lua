-- StackChan servo-bus soak test: is the bus flaky, or is opening the port?
--
-- The symptom this exists for: a `lua --run` of the head skill either works
-- perfectly or sees no servo at all, with nothing in between, while a run a
-- second later behaves differently. Frame-level noise on the wire would show up
-- as a mix inside one run; an all-or-nothing pattern per run points at the port
-- open/teardown path instead.
--
-- So this pings the servos in two phases and compares:
--
--   A. one open, many pings   -- how good is the bus while the port stays up
--   B. open, ping, close, ... -- how good is it across port open/close cycles
--
-- If A is clean and B fails, the wiring is fine and the problem is in opening
-- the UART. If both fail at a similar rate, the bus itself is intermittent.
--
--   lua --run --path /system/scripts/stackchan_bus_soak.lua
--   lua --run --path /system/scripts/stackchan_bus_soak.lua --args-json "{\"cycles\":50}"

local scs = require("lib_scs_servo")
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
local TIMEOUT_MS = int_arg("timeout_ms", 20)
-- Pings per phase. Both phases use the same count so the rates compare directly.
local CYCLES = int_arg("cycles", 30)
local INTERVAL_MS = int_arg("interval_ms", 50)

local IDS = {int_arg("yaw_id", 1), int_arg("pitch_id", 2)}

local function new_bus()
    return scs.new({
        port = PORT, tx = TX, rx = RX, baud = BAUD, timeout_ms = TIMEOUT_MS,
    })
end

-- One ping per id. Returns how many answered plus a compact pattern character,
-- so a run reads as a stream: "." both, "y" yaw only, "p" pitch only, "X" none.
local function ping_round(bus)
    local yaw = bus:ping(IDS[1]) ~= nil
    local pitch = bus:ping(IDS[2]) ~= nil
    local mark
    if yaw and pitch then
        mark = "."
    elseif yaw then
        mark = "y"
    elseif pitch then
        mark = "p"
    else
        mark = "X"
    end
    return yaw, pitch, mark
end

local function report(label, yaw_ok, pitch_ok, marks)
    print(string.format("[soak] %s: yaw %d/%d, pitch %d/%d",
        label, yaw_ok, CYCLES, pitch_ok, CYCLES))
    print("[soak]   " .. table.concat(marks))
end

local bus

local function cleanup()
    if bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

local function run()
    print(string.format("[soak] uart%d tx=%d rx=%d baud=%d, ids %d/%d, %d cycles",
        PORT, TX, RX, BAUD, IDS[1], IDS[2], CYCLES))
    print("[soak] pattern: . both answered, y yaw only, p pitch only, X neither")

    -- Phase A: the port stays open for the whole phase.
    bus = new_bus()
    local a_yaw, a_pitch, a_marks = 0, 0, {}
    for i = 1, CYCLES do
        local yaw, pitch, mark = ping_round(bus)
        if yaw then a_yaw = a_yaw + 1 end
        if pitch then a_pitch = a_pitch + 1 end
        a_marks[#a_marks + 1] = mark
        if i < CYCLES then
            delay.delay_ms(INTERVAL_MS)
        end
    end
    cleanup()
    report("phase A, one open", a_yaw, a_pitch, a_marks)

    -- Phase B: a fresh open and close around every ping, which is what running
    -- the skill repeatedly does.
    local b_yaw, b_pitch, b_marks = 0, 0, {}
    for i = 1, CYCLES do
        bus = new_bus()
        local yaw, pitch, mark = ping_round(bus)
        if yaw then b_yaw = b_yaw + 1 end
        if pitch then b_pitch = b_pitch + 1 end
        b_marks[#b_marks + 1] = mark
        cleanup()
        if i < CYCLES then
            delay.delay_ms(INTERVAL_MS)
        end
    end
    report("phase B, reopened each time", b_yaw, b_pitch, b_marks)

    -- Verdict, stated in terms of what to do next rather than a score.
    local a_total = a_yaw + a_pitch
    local b_total = b_yaw + b_pitch
    local perfect = 2 * CYCLES
    print("[soak] --- verdict ---")
    if a_total == perfect and b_total == perfect then
        print("[soak] both phases clean: the bus and the open path are fine right now.")
        print("[soak]   If the head skill still fails intermittently, the trigger is")
        print("[soak]   something else in that script, not the servo link.")
    elseif a_total == perfect then
        print("[soak] phase A clean, phase B not: the wiring is fine while the port")
        print("[soak]   stays open, and reopening the UART is what breaks it. That is")
        print("[soak]   a firmware problem, not a cable -- send this output.")
    elseif b_total <= a_total then
        print("[soak] both phases lose pings: the bus itself is intermittent. Check the")
        print("[soak]   servo cable and the three connectors, starting at the POWER board.")
    else
        print("[soak] phase A worse than phase B, which is unexpected -- send this output.")
    end
    if a_yaw ~= a_pitch or b_yaw ~= b_pitch then
        print("[soak] the two ids differ, so one servo or its link is worse than the")
        print("[soak]   other; the pattern above shows which.")
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[soak] ERROR: " .. (tostring(err):gsub("\n.*", "")))
    error(err)
end
