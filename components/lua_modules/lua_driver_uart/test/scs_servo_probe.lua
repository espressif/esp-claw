-- Probe Feetech SCS bus servos and optionally nudge them around their current
-- position. Defaults target M5Stack StackChan: UART1, tx=GPIO6, rx=GPIO7,
-- 1 Mbps, yaw = id 1, pitch = id 2.
--
-- The servo power rail must already be up. On StackChan the firmware raises it
-- during board init through the PY32 expander's VM_EN pin; if every ping times
-- out, check that rail first (see test/py32_ioe_basic.lua).

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
local TIMEOUT_MS = int_arg("timeout_ms", 20)
local MOVE_DELTA = int_arg("move_delta", 0)      -- counts; 0 means read-only
local MOVE_TIME = int_arg("move_time", 60)
local SETTLE_MS = int_arg("settle_ms", 600)

local ids = {}
if type(a.ids) == "table" then
    for _, id in ipairs(a.ids) do
        ids[#ids + 1] = math.floor(id)
    end
else
    ids = {int_arg("yaw_id", 1), int_arg("pitch_id", 2)}
end

local bus

local function cleanup()
    if bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

local function report(id)
    local fb, err = bus:read_feedback(id)
    if not fb then
        print(string.format("[scs_servo] id %d feedback failed: %s", id, tostring(err)))
        return nil
    end
    print(string.format(
        "[scs_servo] id %d pos=%d speed=%d load=%d current=%d volt=%d temp=%dC moving=%d",
        id, fb.position, fb.speed, fb.load, fb.current, fb.voltage, fb.temperature, fb.moving))
    return fb
end

local function run()
    bus = scs_servo.new({port = PORT, tx = TX, rx = RX, baud = BAUD, timeout_ms = TIMEOUT_MS})
    print(string.format("[scs_servo] uart%d tx=%d rx=%d baud=%d", PORT, TX, RX, BAUD))

    local present = {}
    for _, id in ipairs(ids) do
        local found, err = bus:ping(id)
        if found then
            present[#present + 1] = id
            print(string.format("[scs_servo] id %d responded", id))
        else
            print(string.format("[scs_servo] id %d no response (%s)", id, tostring(err)))
        end
    end

    if #present == 0 then
        error("no servos responded; check the VM_EN rail, wiring and baud rate")
    end

    local start = {}
    for _, id in ipairs(present) do
        local fb = report(id)
        local torque, terr = bus:read_torque_enable(id)
        print(string.format("[scs_servo] id %d torque=%s", id,
            torque ~= nil and tostring(torque) or ("? " .. tostring(terr))))
        if fb then
            start[id] = fb.position
        end
    end

    if MOVE_DELTA == 0 then
        print("[scs_servo] read-only run; pass move_delta to exercise motion")
        return
    end

    -- Move relative to where each servo already is, then come back, so this is
    -- safe without knowing the machine's home position or travel limits.
    for _, id in ipairs(present) do
        if start[id] then
            bus:enable_torque(id, true)
        end
    end

    for _, delta in ipairs({MOVE_DELTA, -MOVE_DELTA}) do
        local move_ids, positions, times = {}, {}, {}
        for _, id in ipairs(present) do
            if start[id] then
                local target = start[id] + delta
                if target < 0 then target = 0 end
                if target > 1023 then target = 1023 end
                move_ids[#move_ids + 1] = id
                positions[#positions + 1] = target
                times[#times + 1] = MOVE_TIME
            end
        end
        if #move_ids > 0 then
            print(string.format("[scs_servo] moving %d servo(s) by %+d counts", #move_ids, delta))
            bus:sync_write_pos(move_ids, positions, times)
            delay.delay_ms(SETTLE_MS)
            for _, id in ipairs(move_ids) do
                report(id)
            end
        end
    end

    -- Return to the starting positions.
    for _, id in ipairs(present) do
        if start[id] then
            bus:write_pos(id, start[id], MOVE_TIME, 0)
        end
    end
    delay.delay_ms(SETTLE_MS)
    for _, id in ipairs(present) do
        report(id)
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
