-- Phase 1A BLE event queue smoke test.

local ble = require("ble")
local LOG_PREFIX = "[ble_events_test]"
local events = {}

local function log(message)
    print(LOG_PREFIX .. " " .. message)
end

local function assert_ok(result, err)
    if result == nil then
        error(tostring(err))
    end
    return result
end

assert_ok(ble.on_event(function(ev)
    events[#events + 1] = ev
    log("event type=" .. tostring(ev.type) .. " reason=" .. tostring(ev.reason))
end))
log("event callback registered")

assert_ok(ble.init())
log("init ok")
assert_ok(ble.adv_start({ data = { name = "esp-claw" } }))
log("adv_start ok")

local processed = ble.process_events(0)
assert(processed >= 1, "expected at least one event")
assert(events[1].type == "adv_started", "expected adv_started")
assert(events[1].reason == "user_start", "expected user_start")
log("processed start events=" .. tostring(processed)
    .. " first=" .. tostring(events[1].type)
    .. " reason=" .. tostring(events[1].reason))

assert_ok(ble.adv_stop())
log("adv_stop ok")
processed = ble.process_events(0)
assert(processed >= 1, "expected adv_stopped event")
assert(events[#events].type == "adv_stopped", "expected adv_stopped")
assert(events[#events].reason == "user_stop", "expected user_stop")
log("processed stop events=" .. tostring(processed)
    .. " total=" .. tostring(#events)
    .. " last=" .. tostring(events[#events].type)
    .. " reason=" .. tostring(events[#events].reason))

assert_ok(ble.deinit())
log("deinit ok")
assert_ok(ble.deinit())
log("second deinit ok")
assert_ok(ble.on_event(nil))
log("event callback cleared")

local reentrant_events = {}
assert_ok(ble.on_event(function(ev)
    reentrant_events[#reentrant_events + 1] = ev
    log("reentrant event type=" .. tostring(ev.type) .. " reason=" .. tostring(ev.reason))
    if ev.type == "adv_started" then
        local result, err = ble.deinit()
        assert(result ~= nil, tostring(err))
        log("deinit from callback ok")
        assert_ok(ble.on_event(nil))
        log("on_event nil from callback ok")
    end
end))
log("reentrant event callback registered")
assert_ok(ble.init())
log("reentrant init ok")
assert_ok(ble.adv_start({ data = { name = "esp-claw" } }))
log("reentrant adv_start ok")
processed = ble.process_events(0)
assert(processed >= 1, "expected reentrant adv_started event")
assert(reentrant_events[1].type == "adv_started", "expected reentrant adv_started")
stats = ble.stats()
assert(stats.adv_requested == false)
assert(stats.adv_active == false)
log("process_events returned after callback deinit processed=" .. tostring(processed))
assert_ok(ble.on_event(nil))
log("reentrant event callback cleared")

log("Test Passed.")
