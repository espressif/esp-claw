-- Phase 1A BLE bring-up test.
-- Verifies init, stats, events, MTU, advertising, stop, and deinit paths.

local LOG_PREFIX = "[ble_test]"
local ADV_NAME = "esp-claw"
local ADV_HOLD_MS = 5000

local delay_ok, delay = pcall(require, "delay")
local ble_module = nil
local received_events = {}

local function sleep_ms(ms)
    if delay_ok and delay and type(delay.delay_ms) == "function" then
        delay.delay_ms(ms)
        return
    end

    if os and type(os.clock) == "function" then
        local deadline = os.clock() + (ms / 1000)
        while os.clock() < deadline do
        end
        return
    end

    -- Last-resort fallback for very small Lua runtimes without delay or os.clock.
    for _ = 1, ms * 1000 do
    end
end

local function table_to_string(value)
    if type(value) ~= "table" then
        return tostring(value)
    end

    local parts = {}
    for key, item in pairs(value) do
        parts[#parts + 1] = tostring(key) .. "=" .. tostring(item)
    end
    return "{" .. table.concat(parts, ", ") .. "}"
end

local function log(message)
    print(LOG_PREFIX .. " " .. message)
end

local function log_result(step, ok, result, err)
    log(step .. " -> ok=" .. tostring(ok)
        .. " result=" .. table_to_string(result)
        .. " err=" .. tostring(err))
end

local function print_stats(ble, label)
    local ok, stats = pcall(ble.stats)
    log_result("stats " .. label, ok, stats, nil)
    if not ok then
        error(stats)
    end
    return stats
end

local function call_and_stats(ble, step, fn)
    log("step: " .. step)
    local ok, result, err = pcall(fn)
    log_result(step, ok, result, err)
    print_stats(ble, "after " .. step)
    if not ok then
        error(result)
    end
    if result == nil then
        error(step .. " failed: " .. tostring(err))
    end
    return result, err
end

local function run()
    log("require ble")
    local ble = require("ble")
    ble_module = ble
    log("require ble -> " .. tostring(ble))

    assert(type(ble.scan_start) == "nil", "scan_start must not be exposed in Phase 1A")
    assert(type(ble.status) == "nil", "status must not be exposed in Phase 1A")
    assert(type(ble.connection) == "nil", "connection must not be exposed in Phase 1A")

    assert(ble.on_event(function(ev)
        received_events[#received_events + 1] = ev
        log("event " .. tostring(ev.type) .. " reason=" .. tostring(ev.reason))
    end))

    call_and_stats(ble, "ble.init", function()
        return ble.init()
    end)

    call_and_stats(ble, "ble.set_name", function()
        return ble.set_name(ADV_NAME)
    end)

    call_and_stats(ble, "ble.set_max_mtu", function()
        return ble.set_max_mtu(247)
    end)

    call_and_stats(ble, "ble.adv_start", function()
        return ble.adv_start({
            data = { name = ADV_NAME },
        })
    end)
    assert(ble.process_events(0) >= 1)

    log("advertising for " .. tostring(ADV_HOLD_MS) .. " ms; scan with nRF Connect for name " .. ADV_NAME)
    sleep_ms(ADV_HOLD_MS)
    local stats = print_stats(ble, "while advertising")
    assert(stats.adv_requested == true)
    assert(stats.adv_active == true)
    assert(type(stats.mac) == "string" and #stats.mac == 6)
    assert(type(stats.connections) == "table" and #stats.connections == 3)
    assert(stats.connections[1].conn_index == 0)

    call_and_stats(ble, "ble.adv_stop", function()
        return ble.adv_stop()
    end)
    assert(ble.process_events(0) >= 1)
    log("received events total=" .. tostring(#received_events)
        .. " first=" .. tostring(received_events[1] and received_events[1].type)
        .. " last=" .. tostring(received_events[#received_events] and received_events[#received_events].type))

    call_and_stats(ble, "ble.deinit", function()
        return ble.deinit()
    end)

    assert(#received_events >= 2, "expected adv_started and adv_stopped events")

    log("Test Passed.")
end

local function cleanup()
    if ble_module then
        pcall(ble_module.adv_stop)
        pcall(ble_module.deinit)
        ble_module = nil
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    log("ERROR: " .. tostring(err))
    error(err)
end
