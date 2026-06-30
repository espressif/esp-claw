-- BLE module entry point for Agent calls (Phase 1C).
--
-- This script initialises NimBLE, starts connectable Legacy Advertising, and
-- runs an event loop until the Lua runtime requests a stop.
--
-- Usage:
--   lua --run --path builtin/skills/ble/scripts/start_ble.lua
--
-- Configuration: edit the table below before running.

local CONFIG = {
    device_name     = "esp-claw",
    interval_min_ms = 100,
    interval_max_ms = 200,
    mtu             = 64,
    event_poll_ms   = 100,
    gatt_service    = "fff0",
    gatt_char       = "fff1",
    gatt_char_id    = "rx_tx",
    smp             = {
        bonding = true,
        secure_connections = true,
        mitm = false,
        io_cap = "no_io",
        key_dist = { enc = true, id = true },
        repeat_pairing = "delete_retry",
        require_bond_persist = false,
    },
}

local ble = require("ble")

local function log(msg)
    print("[start_ble] " .. msg)
end

local function assert_ok(ok, err)
    if not ok then
        error(tostring(err))
    end
    return ok
end

-- Event handler: prints relevant events for diagnostics.
local function on_event(ev)
    local t = ev.type
    if t == "adv_started" then
        log("adv_started reason=" .. tostring(ev.reason))
    elseif t == "adv_stopped" then
        log("adv_stopped reason=" .. tostring(ev.reason)
            .. (ev.error_code and (" error_code=" .. tostring(ev.error_code)) or ""))
    elseif t == "connected" then
        log("connected conn_index=" .. tostring(ev.conn_index)
            .. " peer_addr_type=" .. tostring(ev.peer_addr_type))
    elseif t == "disconnected" then
        log("disconnected conn_index=" .. tostring(ev.conn_index)
            .. " reason=" .. tostring(ev.reason))
    elseif t == "mtu_changed" then
        log("mtu_changed conn_index=" .. tostring(ev.conn_index)
            .. " mtu=" .. tostring(ev.mtu))
    elseif t == "security_changed" then
        log("security_changed conn_index=" .. tostring(ev.conn_index)
            .. " encrypted=" .. tostring(ev.encrypted)
            .. " bonded=" .. tostring(ev.bonded))
    elseif t == "gatts_read" then
        log("gatts_read conn_index=" .. tostring(ev.conn_index)
            .. " characteristic=" .. tostring(ev.characteristic_id or ev.uuid_characteristic))
    elseif t == "gatts_write" then
        log("gatts_write conn_index=" .. tostring(ev.conn_index)
            .. " characteristic=" .. tostring(ev.characteristic_id or ev.uuid_characteristic)
            .. " len=" .. tostring(ev.data and #ev.data or 0))
    elseif t == "subscribe_changed" then
        log("subscribe_changed conn_index=" .. tostring(ev.conn_index)
            .. " characteristic=" .. tostring(ev.characteristic_id or ev.uuid_characteristic)
            .. " notify=" .. tostring(ev.notify)
            .. " indicate=" .. tostring(ev.indicate))
    elseif t == "notify_complete" or t == "indicate_complete" then
        log(t .. " conn_index=" .. tostring(ev.conn_index)
            .. " characteristic=" .. tostring(ev.characteristic_id or ev.uuid_characteristic)
            .. " status=" .. tostring(ev.status))
    end
end

-- Configure SMP before initialising BLE. Phase 1 does not expose passkey,
-- numeric-comparison, or bond-management Lua APIs.
assert_ok(ble.smp_config(CONFIG.smp))

-- Initialise BLE stack.
log("initialising BLE stack")
assert_ok(ble.init())

-- Register event callback before advertising so no events are missed.
assert_ok(ble.on_event(on_event))

-- Set preferred MTU.
assert_ok(ble.set_max_mtu(CONFIG.mtu))

-- Set GAP device name.
assert_ok(ble.set_name(CONFIG.device_name))

-- Define a simple dynamic GATT service. Update cached values with
-- ble.gatts_set_value(), then call ble.notify()/ble.indicate() after a peer
-- subscribes.
assert_ok(ble.gatts_define({
    services = {
        {
            uuid = CONFIG.gatt_service,
            characteristics = {
                {
                    uuid = CONFIG.gatt_char,
                    id = CONFIG.gatt_char_id,
                    properties = {
                        read = true,
                        write = true,
                        write_no_rsp = true,
                        notify = true,
                        indicate = true,
                    },
                    permissions = {
                        read = true,
                        write = true,
                    },
                    value = "hello",
                    max_len = 128,
                    descriptors = {
                        {
                            uuid = "2901",
                            permissions = { read = true },
                            value = "RX/TX channel",
                        },
                    },
                },
            },
        },
    },
}))

-- Start advertising.
log("starting advertising as '" .. CONFIG.device_name .. "'")
assert_ok(ble.adv_start({
    connectable     = true,
    scannable       = true,
    interval_min_ms = CONFIG.interval_min_ms,
    interval_max_ms = CONFIG.interval_max_ms,
    data            = {
        name = CONFIG.device_name,
        service_uuids = { CONFIG.gatt_service },
    },
}))

-- Print initial stats.
local stats = ble.stats()
log("mac=" .. string.format("%02x:%02x:%02x:%02x:%02x:%02x",
    stats.mac:byte(1), stats.mac:byte(2), stats.mac:byte(3),
    stats.mac:byte(4), stats.mac:byte(5), stats.mac:byte(6)))
log("adv_active=" .. tostring(stats.adv_active)
    .. " preferred_mtu=" .. tostring(stats.preferred_mtu))

-- Event loop: poll until the Lua runtime requests a stop.
log("entering event loop (poll every " .. CONFIG.event_poll_ms .. " ms)")
while true do
    ble.process_events(CONFIG.event_poll_ms)
end
