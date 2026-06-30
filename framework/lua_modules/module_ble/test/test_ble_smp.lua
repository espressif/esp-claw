-- Phase 1C BLE SMP configuration smoke test.

local ble = require("ble")
local LOG_PREFIX = "[ble_smp_test]"

local function log(message)
    print(LOG_PREFIX .. " " .. message)
end

local function assert_ok(result, err)
    if result == nil then
        error(tostring(err))
    end
    return result
end

local bad, err = ble.smp_config({
    mitm = true,
    io_cap = "no_io",
})
assert(bad == nil and err == "ble_smp_invalid_config")
log("mitm with no_io rejected -> " .. tostring(err))

assert_ok(ble.init())
log("init ok")
bad, err = ble.smp_config({})
assert(bad == nil and err == "ble_invalid_state")
log("smp_config after init rejected -> " .. tostring(err))

bad, err = ble.gatts_define({
    services = {
        {
            uuid = "fff0",
            characteristics = {
                {
                    uuid = "fff1",
                    id = "auth_required",
                    properties = { read = true },
                    permissions = { read_authenticated = true },
                },
            },
        },
    },
})
assert(bad == nil and err == "ble_smp_invalid_config")
log("authenticated GATT permission rejected -> " .. tostring(err))
assert_ok(ble.deinit())
log("deinit ok after authenticated permission check")

bad, err = ble.smp_config({
    bonding = true,
    secure_connections = true,
    mitm = true,
    io_cap = "display_yes_no",
    key_dist = {
        enc = true,
        id = true,
    },
    repeat_pairing = "delete_retry",
    require_bond_persist = false,
})
assert(bad == nil and err == "ble_smp_unsupported_io")
log("unsupported io_cap display_yes_no rejected -> " .. tostring(err))

assert_ok(ble.smp_config({
    bonding = true,
    secure_connections = true,
    mitm = false,
    io_cap = "no_io",
    repeat_pairing = "ignore",
}))
log("final no_io SMP config accepted repeat_pairing=ignore")

log("Test Passed.")
