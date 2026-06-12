# Lua BLE

This document is written for LLMs and Lua script generation. It explains how
to use the `ble` module exposed by `components/lua_modules/lua_module_ble` for
ordinary BLE Peripheral advertising and GATT Server tasks.

## When To Use

- Use this module for normal BLE advertising, GATT service discovery,
  characteristic read/write, notify, and indicate.
- Use `lua_module_ble_hid` only when the user explicitly asks for HID keyboard,
  mouse, media keys, typing, key combos, cursor movement, clicking, or scrolling.
- Do not use `ble_hid` as a fallback for ordinary BLE advertising or GATT tasks.

## Core Rules

- Import the module with `local ble = require("ble")`.
- `lua_module_ble` and `lua_module_ble_hid` must not advertise at the same time.
- To switch from ordinary BLE to HID, call `ble.adv_stop()` then `ble.deinit()`
  before starting `lua_module_ble_hid`.
- To switch from HID to ordinary BLE, stop and deinitialize `lua_module_ble_hid`
  before calling `ble.init()` / `ble.adv_start(...)`.
- Call `ble.smp_config(opts)` before `ble.init()` when custom security is needed.
- Call `ble.init()` before setting name, MTU, GATT, advertising, events, or stats.
- Register events with `ble.on_event(fn)`, then call `ble.process_events(...)`
  in a loop; otherwise Lua callbacks will not run.
- Define a GATT profile once with `ble.gatts_define(profile)`. Hot modification
  is not supported; call `ble.deinit()` before defining a different profile.
- Use stable `conn_index` values `0..2` for peer operations. `conn_handle` is
  diagnostic only.
- Call `ble.notify()` or `ble.indicate()` only after a peer connects and
  subscribes to the characteristic.
- Unsupported: scanning, observer mode, Central mode, GATT Client, extended
  advertising, passkey / numeric-comparison Lua callbacks, and Lua
  bond-management APIs.

## How To Call

- `ble.smp_config(opts)`: configure security before `ble.init()`.
- `ble.init()`: start the BLE controller and NimBLE host. Idempotent.
- `ble.deinit()`: stop advertising, disconnect peers, drain events, and release
  this module's BLE state. Idempotent.
- `ble.set_name(name)`: set the GAP device name. `name` must be 28 bytes or less.
- `ble.set_max_mtu(mtu)`: set preferred ATT MTU from `23..517` before connections.
- `ble.gatts_define(profile)`: define and activate a dynamic GATT Server.
- `ble.gatts_set_value(opts)`: update a characteristic cached value without
  notifying peers.
- `ble.adv_start(opts)`: start Legacy Advertising.
- `ble.adv_stop()`: stop advertising and disable automatic re-advertising.
- `ble.on_event(fn_or_nil)`: register or clear the single BLE event callback.
- `ble.process_events(timeout_ms)`: dispatch up to 8 queued events.
- `ble.stats([opts])`: inspect module state, connections, or cached characteristic
  values.
- `ble.disconnect([opts])`: disconnect a peer by `conn_index`; the index may be
  omitted only when exactly one peer is connected.
- `ble.notify(opts)`: send a notification to a subscribed peer.
- `ble.indicate(opts)`: send an indication to a subscribed peer.

Most operations return `true` on success or `nil, err` on recoverable runtime
failure. Parameter type and shape errors raise Lua errors.

## Minimal Advertising Example

Prefer the runtime script when the agent only needs to start ordinary BLE
advertising. It initializes BLE, defines a simple `fff0/fff1` GATT service,
starts advertising, and runs an event loop:

```json
{"path":"/fatfs/skills/ble/scripts/start_ble.lua","args":{},"timeout_ms":0}
```

Stop ordinary BLE advertising with:

```json
{"path":"/fatfs/skills/ble/scripts/stop_ble.lua","args":{},"timeout_ms":5000}
```

Use direct Lua when generating a custom advertising script:

```lua
local ble = require("ble")

assert(ble.init())
assert(ble.set_name("esp-claw"))
assert(ble.adv_start({
    data = {
        name = "esp-claw",
    },
}))

while true do
    ble.process_events(100)
end
```

## Minimal GATT Example

Use the same runtime start/stop scripts as the advertising example when the
default `fff0/fff1` GATT service is enough. Use direct Lua when the generated
script needs a custom GATT profile or event behavior.

This example exposes service `fff0` with one read/write/notify characteristic.
A peer must subscribe before `ble.notify()` can succeed.

```lua
local ble = require("ble")

local subscribed = {}

assert(ble.init())
assert(ble.set_name("esp-claw"))

assert(ble.gatts_define({
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
                        notify = true,
                    },
                    permissions = {
                        read = true,
                        write = true,
                    },
                    value = "hello",
                    max_len = 128,
                },
            },
        },
    },
}))

assert(ble.on_event(function(ev)
    if ev.type == "gatts_write" and ev.characteristic_id == "rx_tx" then
        ble.gatts_set_value({ characteristic_id = "rx_tx", data = ev.data })
        if subscribed[ev.conn_index] then
            ble.notify({
                conn_index = ev.conn_index,
                characteristic = "rx_tx",
                data = "echo:" .. ev.data,
            })
        end
    elseif ev.type == "subscribe_changed" and ev.characteristic_id == "rx_tx" then
        subscribed[ev.conn_index] = ev.notify or ev.indicate
    elseif ev.type == "disconnected" then
        subscribed[ev.conn_index] = nil
    end
end))

assert(ble.adv_start({
    data = {
        name = "esp-claw",
        service_uuids = { "fff0" },
    },
}))

while true do
    ble.process_events(100)
end
```

Common advertising fields:

```lua
ble.adv_start({
    data = {
        name = "esp-claw",
        service_uuids = { "fff0" },
        manufacturer_data = "\x01\x02",
    },
    scan_response = {
        name = "esp-claw",
    },
})
```

Advertising payloads are Legacy Advertising payloads and must fit the 31-byte AD
data limit. `adv_start()` sets `adv_requested = true`; advertising automatically
restarts after a disconnect if a connection slot becomes free. `ble.adv_stop()`
clears `adv_requested`.

UUIDs may be 16-bit, 32-bit, or 128-bit strings. Prefer stable characteristic
`id` values for lookup. Do not declare descriptor `2902`; NimBLE creates CCCD
automatically for notify and indicate characteristics.

## Events

Register one global callback with `ble.on_event(fn)`. Events are delivered only
when Lua calls `ble.process_events(timeout_ms)`.

| `ev.type` | Key fields |
|-----------|------------|
| `adv_started` | `reason` = `"user_start"` or `"re_advertise"` |
| `adv_stopped` | `reason` = `"user_stop"`, `"connection_full"`, `"complete"`, `"deinit"`, or `"error"`; `error_code` on errors |
| `connected` | `conn_index`, `conn_handle`, `peer_addr`, `peer_addr_type` |
| `disconnected` | `conn_index`, `reason` = `"local"`, `"remote"`, `"timeout"`, `"deinit"`, or `"error"` |
| `mtu_changed` | `conn_index`, `mtu` |
| `security_changed` | `conn_index`, `encrypted`, `authenticated`, `bonded`, `key_size`, `reason` |
| `gatts_read` | `conn_index`, `service_id`, `characteristic_id`, `uuid_service`, `uuid_characteristic`, `uuid_descriptor`, `offset` |
| `gatts_write` | `conn_index`, `service_id`, `characteristic_id`, `uuid_service`, `uuid_characteristic`, `uuid_descriptor`, `data`, `offset` |
| `subscribe_changed` | `conn_index`, `service_id`, `characteristic_id`, `uuid_service`, `uuid_characteristic`, `notify`, `indicate` |
| `notify_complete` | `conn_index`, `characteristic_id`, `status`, `error_code` |
| `indicate_complete` | `conn_index`, `characteristic_id`, `status`, `error_code` |

## Security

Default security is bonding enabled, Secure Connections enabled, MITM disabled
and Just Works pairing with no input/output capability.

Call `ble.smp_config()` before `ble.init()` to change policy:

```lua
ble.smp_config({
    bonding = true,
    secure_connections = true,
    mitm = false,
    io_cap = "no_io", -- "no_io" | "display_only" | "display_yes_no" | "keyboard_only" | "keyboard_display"
    key_dist = { enc = true, id = true },
    repeat_pairing = "delete_retry", -- "ignore" | "delete_retry"
    require_bond_persist = false,
})
```

`mitm = true` requires `io_cap` other than `"no_io"`, otherwise the call returns
`nil, "ble_smp_invalid_config"`. GATT permissions using `read_authenticated` or
`write_authenticated` also require a MITM-capable SMP config. Encrypted-only
permissions work with the default Just Works policy.

Bond persistence requires `CONFIG_BT_NIMBLE_NVS_PERSIST=y`. If
`require_bond_persist = true` and persistence is disabled, `ble.init()` returns
`nil, "ble_smp_nvs_persist_disabled"`.

## Stats And Connections

`ble.stats()` returns module state:

```lua
local stats = ble.stats()
print(stats.adv_active, stats.adv_requested, stats.preferred_mtu)

for _, conn in ipairs(stats.connections) do
    print(conn.conn_index, conn.connected, conn.encrypted, conn.mtu)
end
```

`stats.connections` always has 3 Lua table entries. The table is indexed with
Lua's normal 1-based indexes, but each connection entry exposes stable
`conn_index` values `0..2` for calls such as `ble.disconnect()`, `ble.notify()`,
and `ble.indicate()`.

Read a cached characteristic value with:

```lua
local char = ble.stats({
    char = {
        characteristic_id = "rx_tx",
    },
})
print(char.value)
```

## Common Errors

Successful mutating operations usually return `true`. Runtime failures return
`nil, err`. Parameter validation errors raise Lua errors.

Common recoverable errors:

- `ble_init_not_initialized`: call `ble.init()` first.
- `ble_invalid_state`: operation is not valid in the current state.
- `ble_conn_index_required`: multiple peers are connected; pass `conn_index`.
- `ble_conn_not_found`: no peer exists at that `conn_index`.
- `ble_adv_data_too_long`: advertising payload exceeds 31 bytes.
- `ble_gatt_char_not_found`: characteristic lookup failed.
- `ble_gatt_char_ambiguous`: UUID lookup matched multiple characteristics.
- `ble_gatt_not_subscribed`: peer has not subscribed to notify or indicate.
- `ble_gatt_indicate_in_progress`: connection already has a pending indication.
- `ble_smp_invalid_config`: SMP config or authenticated permission is invalid.
- `ble_smp_security_failed`: secure notify or indicate was attempted before the
  connection met the required security level.
- `ble_smp_nvs_persist_disabled`: bond persistence was required but is disabled.
