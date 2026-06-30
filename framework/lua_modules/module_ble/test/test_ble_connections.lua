-- Phase 1A BLE stats and connection table smoke test.

local ble = require("ble")
local LOG_PREFIX = "[ble_connections_test]"

local function log(message)
    print(LOG_PREFIX .. " " .. message)
end

local function assert_ok(result, err)
    if result == nil then
        error(tostring(err))
    end
    return result
end

assert_ok(ble.init())
log("init ok")

local stats = ble.stats()
assert(type(stats) == "table")
assert(type(stats.mac) == "string" and #stats.mac == 6)
assert(type(stats.connections) == "table" and #stats.connections == 3)
log("stats mac_len=" .. tostring(#stats.mac)
    .. " connections=" .. tostring(#stats.connections)
    .. " adv_requested=" .. tostring(stats.adv_requested)
    .. " adv_active=" .. tostring(stats.adv_active))
for i = 1, 3 do
    assert(stats.connections[i].conn_index == i - 1)
    assert(stats.connections[i].connected == false)
end
log("connection slots verified disconnected")

assert_ok(ble.set_max_mtu(517))
log("set_max_mtu 517 ok")
local bad, err = ble.set_max_mtu(22)
assert(bad == nil and err == "ble_mtu_invalid")
log("set_max_mtu invalid -> " .. tostring(err))

bad, err = ble.adv_start({ mode = "extended", data = { name = "esp-claw" } })
assert(bad == nil and err == "ble_unsupported")
log("extended advertising unsupported -> " .. tostring(err))

local schema_ok = pcall(function()
    ble.adv_start({
        data = { name = "esp-claw" },
        data_tlv = {
            { ad_type = ble.ADV_TYPE.MANUFACTURER_SPECIFIC_DATA, value = "raw" },
        },
    })
end)
assert(schema_ok == false, "data and data_tlv must be mutually exclusive")
log("adv data/data_tlv conflict rejected")

bad, err = ble.adv_start({
    data = {
        name = "esp-claw",
        manufacturer_data = string.rep("x", 31),
    },
})
assert(bad == nil and err == "ble_adv_data_too_long")
log("oversized advertising data -> " .. tostring(err))

assert_ok(ble.adv_start({ data = { name = "esp-claw" } }))
stats = ble.stats()
assert(stats.adv_requested == true)
assert(stats.adv_active == true)
log("advertising active adv_requested=" .. tostring(stats.adv_requested)
    .. " adv_active=" .. tostring(stats.adv_active))

local disconnected, disconnect_err = ble.disconnect({ conn_index = 0 })
assert(disconnected == nil and disconnect_err == "ble_conn_not_found")
log("disconnect empty slot -> " .. tostring(disconnect_err))

assert_ok(ble.adv_stop())
log("adv_stop ok")
assert_ok(ble.deinit())
log("deinit ok")

log("Test Passed.")
