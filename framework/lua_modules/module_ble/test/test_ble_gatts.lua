-- Phase 1B BLE dynamic GATT Server smoke test.

local ble = require("ble")
local LOG_PREFIX = "[ble_gatts_test]"

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

assert_ok(ble.gatts_define({
    services = {
        {
            uuid = "fff0",
            characteristics = {
                {
                    uuid = "fff1",
                    id = "rx_tx",
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
                {
                    uuid = "12345678",
                    id = "uuid32",
                    properties = { read = true },
                    permissions = { read = true },
                    value = "uuid32-ok",
                },
            },
        },
    },
}))
log("primary GATT profile defined services=1 chars=2 descriptor=2901")

local char = ble.stats({ char = { service_id = "fff0", characteristic_id = "rx_tx" } })
assert(type(char) == "table" and char.value == "hello")
log("rx_tx initial value=" .. tostring(char.value))

assert_ok(ble.gatts_set_value({
    characteristic_id = "rx_tx",
    data = "updated",
}))
char = ble.stats({ char = { characteristic_id = "rx_tx" } })
assert(char.value == "updated")
log("rx_tx updated value=" .. tostring(char.value))

local bad, err = ble.gatts_set_value({
    characteristic_id = "rx_tx",
    data = string.rep("x", 129),
})
assert(bad == nil and err == "ble_gatt_data_too_long")
log("oversized gatts_set_value -> " .. tostring(err))

bad, err = ble.gatts_define({
    services = {
        {
            uuid = "fff2",
            characteristics = {
                {
                    uuid = "fff3",
                    properties = { read = true },
                    permissions = { read = true },
                },
            },
        },
    },
})
assert(bad == nil and err == "ble_gatt_already_started")
log("duplicate gatts_define while active -> " .. tostring(err))

bad, err = ble.notify({
    characteristic = "rx_tx",
    data = "hello",
})
assert(bad == nil and err == "ble_conn_not_found")
log("notify without connection -> " .. tostring(err))

assert_ok(ble.deinit())
log("deinit after primary profile ok")

assert_ok(ble.init())
log("init for unsupported secondary service case ok")
bad, err = ble.gatts_define({
    services = {
        {
            uuid = "fff0",
            secondary = true,
            characteristics = {
                {
                    uuid = "fff1",
                    properties = { read = true },
                    permissions = { read = true },
                },
            },
        },
    },
})
assert(bad == nil and err == "ble_unsupported")
log("secondary service rejected -> " .. tostring(err))
assert_ok(ble.deinit())
log("deinit after secondary service case ok")

assert_ok(ble.init())
log("init for authenticated permission case ok")
bad, err = ble.gatts_define({
    services = {
        {
            uuid = "fff0",
            characteristics = {
                {
                    uuid = "fff1",
                    id = "secure",
                    properties = { read = true },
                    permissions = { read_authenticated = true },
                },
            },
        },
    },
})
assert(bad == nil and err == "ble_smp_invalid_config")
log("authenticated permission rejected -> " .. tostring(err))
assert_ok(ble.deinit())
log("deinit after authenticated permission case ok")

assert_ok(ble.init())
log("init for CCCD schema case ok")
local ok = pcall(function()
    ble.gatts_define({
        services = {
            {
                uuid = "fff0",
                characteristics = {
                    {
                        uuid = "fff1",
                        properties = { notify = true },
                        descriptors = {
                            { uuid = "2902", permissions = { read = true } },
                        },
                    },
                },
            },
        },
    })
end)
assert(ok == false, "declaring CCCD descriptor 2902 must be a schema error")
log("manual CCCD descriptor rejected pcall_ok=" .. tostring(ok))
assert_ok(ble.deinit())
log("deinit after CCCD schema case ok")

log("Test Passed.")
