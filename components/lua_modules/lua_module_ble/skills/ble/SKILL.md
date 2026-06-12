---
{
  "name": "ble",
  "description": "Operate ESP-Claw BLE: initialize BLE, advertise, handle connections, process events, and inspect module state.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# BLE Skill (lua_module_ble · Phase 1C)

This skill describes how an Agent can control the BLE module on ESP-Claw via
Lua scripts. The module is a thin ESP-IDF NimBLE adapter; it does not depend on
LuatOS, Arduino, or any intermediate framework.

---

## Capabilities (Phase 1C)

- BLE controller and NimBLE host bring-up / teardown
- Legacy (BT4) connectable advertising with device name, appearance, and 16-bit
  service UUIDs
- Fixed 3-slot connection table with stable `conn_index` identifiers
- MTU preference and per-connection MTU tracking
- Unified event queue: `ble.on_event(fn)` + `ble.process_events(timeout_ms)`
- Peripheral-side disconnect (`ble.disconnect`)
- Module diagnostics via `ble.stats()`
- Dynamic GATT Server via `ble.gatts_define(profile)`
- Cached characteristic values via `ble.gatts_set_value()` and
  `ble.stats({ char = ... })`
- Server-initiated `ble.notify()` / `ble.indicate()` using stable `conn_index`
- SMP configuration via `ble.smp_config(opts)` before `ble.init()`

**Not in Phase 1:** Central / scanning / GATT Client, passkey / numeric
comparison Lua callbacks, bond-management Lua APIs, BLE 5.x extended
advertising.

---

## Mutual Exclusion with `lua_module_ble_hid`

### Skill selection hard rule

When the user asks only for "broadcasting", "advertising", "normal BLE",
"BLE", "GATT", service discovery, characteristic read/write, notify, or
indicate, use only this `ble` skill. **Do not use `ble_hid` as a fallback for
ordinary BLE advertising or GATT tasks.**

Use `ble_hid` only when the user explicitly mentions HID, keyboard, mouse,
media controls, consumer control, typing text, key combos, cursor movement,
clicking, or scrolling.

**Hard rule: `lua_module_ble` and `lua_module_ble_hid` must never advertise
simultaneously.** Both modules share the single BLE controller and NimBLE host;
running both at the same time causes undefined behaviour.

Switching from BLE → HID:
1. Call `ble.adv_stop()` then `ble.deinit()`.
2. Start `lua_module_ble_hid` (e.g. via `skills/ble_hid/scripts/start_ble_hid.lua`).
3. Inform the user that the mode has changed.

Switching from HID → BLE:
1. Deinitialize `lua_module_ble_hid` via its own deinit API.
2. Then call `ble.init()` / `ble.adv_start(...)` in this module.
3. Inform the user.

---

## Quick-start entry point

Run `skills/ble/scripts/start_ble.lua` to initialise BLE and start advertising.
The script runs an indefinite `process_events` loop; send a stop signal to
terminate it.

```text
lua --run --path builtin/skills/ble/scripts/start_ble.lua
```

To stop ordinary BLE advertising and tear down this module, run
`skills/ble/scripts/stop_ble.lua`. Do not try HID scripts as a recovery path for
ordinary BLE failures.

```text
lua --run --path builtin/skills/ble/scripts/stop_ble.lua
```

---

## API Reference (Phase 1C)

All functions that can fail return `true` on success or `nil, "error_code"` on
failure (recoverable errors). Parameter type errors raise `luaL_error`.

### `ble.init() → true | nil, err`

Starts BLE controller (if not already running) and NimBLE host. Waits up to
15 s for host sync. Idempotent.

Error codes: `ble_unsupported` (if `CONFIG_BT_NIMBLE_MAX_CONNECTIONS < 1`).

If `ble.smp_config({ require_bond_persist = true })` was set and NimBLE NVS
persistence is disabled, returns `ble_smp_nvs_persist_disabled`.

### `ble.smp_config(opts) → true | nil, err`

Configures NimBLE SMP before the BLE stack is initialized:

```lua
ble.smp_config({
  bonding = true,
  secure_connections = true,
  mitm = false,
  io_cap = "no_io",
  key_dist = { enc = true, id = true },
  repeat_pairing = "delete_retry",
  require_bond_persist = false,
})
```

`io_cap` must be one of `"no_io"`, `"display_only"`, `"display_yes_no"`,
`"keyboard_only"`, `"keyboard_display"`. `mitm = true` with `"no_io"` returns
`ble_smp_invalid_config`. Calling this after `ble.init()` returns
`ble_invalid_state`.

### `ble.deinit() → true | nil, err`

Drains the event queue (firing all pending callbacks), stops advertising,
disconnects peers, stops NimBLE host, releases only controller state owned by
this module. Idempotent. Safe to call from any state.

### `ble.set_name(name) → true | nil, err`

Sets the GAP Device Name. `name` must be a string ≤ 28 bytes. Requires
`ble.init()` to have been called. Becomes the default advertising name when
`adv_start()` is called without `data.name`.

### `ble.set_max_mtu(mtu) → true | nil, err`

Sets the preferred ATT MTU (23..517). Must be called after `ble.init()` and
before any active connection exists.

Error codes: `ble_mtu_invalid`, `ble_init_not_initialized`,
`ble_invalid_state` (connection already exists).

### `ble.adv_start(opts) → true | nil, err`

Starts Legacy Advertising. `opts` is a table:

```lua
ble.adv_start({
  -- advertising properties
  connectable     = true,          -- default true
  scannable       = true,          -- default true
  directed        = false,         -- true requires directed_addr
  -- timing
  interval_min_ms = 100,
  interval_max_ms = 200,
  addr_mode       = "public",      -- "public" | "random"
  channel_map     = "all",
  -- AD payload (high-level; mutually exclusive with data_tlv)
  data = {
    name             = "esp-claw",
    service_uuids    = { "fff0" },
    manufacturer_data = "\x01\x02",
    appearance       = 0x03C0,
    tx_power         = -4,
  },
  -- scan response (same fields as data)
  scan_response = {
    name = "esp-claw",
  },
  -- raw TLV escape hatch (mutually exclusive with data)
  -- data_tlv = {
  --   { ad_type = ble.ADV_TYPE.MANUFACTURER_SPECIFIC_DATA, value = "raw" },
  -- },
})
```

Error codes: `ble_adv_busy`, `ble_adv_data_too_long`, `ble_unsupported` (BLE
5.x fields), `ble_init_not_initialized`.

`adv_requested` state machine: after `adv_start()` the module maintains
`adv_requested = true`. If a 3rd connection makes the slot full, advertising is
paused (`adv_stopped reason="connection_full"`) but `adv_requested` stays true.
When a slot frees up, advertising restarts automatically
(`adv_started reason="re_advertise"`).

### `ble.adv_stop() → true | nil, err`

Stops advertising. Sets `adv_requested = false`. Idempotent.

### `ble.disconnect(opts) → true | nil, err`

Disconnects a peer from the Peripheral side.

```lua
ble.disconnect({ conn_index = 0 })
```

`conn_index` may be omitted when exactly one connection is active. On success,
a `disconnected reason="local"` event is queued asynchronously.

Error codes: `ble_conn_not_found`, `ble_conn_index_required` (multiple
connections, no index given), `ble_conn_invalid_index`.

### `ble.on_event(fn_or_nil) → true | nil, err`

Registers (or clears) the single global BLE event callback. Replacing the
callback also clears the pending event queue.

`fn_or_nil` must be a function or nil; other types raise `luaL_error`.

### `ble.process_events(timeout_ms) → number | nil, err`

Dispatches up to 8 pending events on the Lua task. `timeout_ms` (default 0)
sets how long to wait for the first event; subsequent reads are non-blocking.
Returns the number of events dispatched.

### `ble.stats(opts?) → table | nil, err`

Returns a module snapshot. Without arguments:

```lua
{
  mac          = "\xAA...",   -- 6-byte raw MAC string
  preferred_mtu = 64,
  adv_requested = true,
  adv_active    = true,
  event_dropped = 0,
  connections   = {
    { conn_index = 0, connected = true, peer_addr = "...", peer_addr_type = "public",
      encrypted = true, authenticated = false, bonded = true, key_size = 16, mtu = 64 },
    { conn_index = 1, connected = false },
    { conn_index = 2, connected = false },
  },
}
```

`connections` always has exactly 3 entries (indexed 1..3 in Lua; `conn_index`
is 0..2).

With `char` argument (Phase 1B+, returns characteristic cached value):

```lua
ble.stats({ char = { service_id = "fff0", characteristic_id = "rx_tx" } })
→ { value = "..." }
```

### `ble.gatts_define(profile) → true | nil, err`

Defines and immediately activates a GATT Server. Call after `ble.init()` and
before a GATT profile is active. Hot modification is unsupported; call
`ble.deinit()` before defining a different profile.

```lua
ble.gatts_define({
  services = {
    {
      uuid = "fff0",
      characteristics = {
        {
          uuid = "fff1",
          id = "rx_tx",
          properties = { read = true, write = true, write_no_rsp = true, notify = true, indicate = true },
          permissions = { read = true, write = true },
          value = "hello",
          max_len = 128,
          descriptors = {
            { uuid = "2901", permissions = { read = true }, value = "RX/TX channel" },
          },
        },
      },
    },
  },
})
```

UUIDs can be 16-bit, 32-bit, or 128-bit strings. Characteristic lookup prefers
the stable `id`; UUID lookup is allowed but returns `ble_gatt_char_ambiguous`
when multiple characteristics match. Do not declare descriptor `2902` because
NimBLE creates CCCD automatically for notify / indicate.

Error codes: `ble_init_not_initialized`, `ble_gatt_already_started`,
`ble_unsupported` for `secondary = true`, `ble_smp_invalid_config` for
authenticated permissions without a MITM-capable SMP config.

### `ble.gatts_set_value(opts) → true | nil, err`

Updates the C-side cached value. It does not notify peers.

```lua
ble.gatts_set_value({
  service_id = "fff0",            -- optional when characteristic is unique
  characteristic_id = "rx_tx",
  data = "hello",
})
```

Error codes: `ble_invalid_state`, `ble_gatt_char_not_found`,
`ble_gatt_char_ambiguous`, `ble_gatt_data_too_long`.

### `ble.notify(opts) → true | nil, err`

Sends a notification to a peer that has subscribed to the characteristic.

```lua
ble.notify({
  conn_index = 0,                 -- optional only when exactly one connection exists
  characteristic = "rx_tx",
  data = "hello",
})
```

Error codes: `ble_conn_not_found`, `ble_conn_index_required`,
`ble_conn_invalid_index`, `ble_gatt_char_not_found`,
`ble_gatt_not_subscribed`, `ble_gatt_data_too_long`,
`ble_gatt_send_failed`, `ble_smp_security_failed`.

### `ble.indicate(opts) → true | nil, err`

Same shape as `ble.notify()`, but sends an indication and waits for ATT
confirmation. Each connection can have only one pending indication; a second
call returns `ble_gatt_indicate_in_progress` until `indicate_complete` arrives.

---

## Module Constants

```lua
ble.ADV_TYPE.FLAGS                     -- 0x01
ble.ADV_TYPE.COMPLETE_LOCAL_NAME       -- 0x09
ble.ADV_TYPE.MANUFACTURER_SPECIFIC_DATA -- 0xFF
-- ... (full list in source)

ble.PERM.READ                          -- 0x08
ble.PERM.WRITE                         -- 0x10
ble.PERM.INDICATE                      -- 0x20
ble.PERM.NOTIFY                        -- 0x40
ble.PERM.WRITE_NO_RSP                  -- 0x80

ble.ADDR_MODE.PUBLIC                   -- "public"
ble.ADDR_MODE.RANDOM                   -- "random"
```

---

## Events

Register one global callback with `ble.on_event(fn)` and poll it with
`ble.process_events(timeout_ms)`:

```lua
ble.on_event(function(ev)
  if ev.type == "connected" then
    print("connected conn_index=" .. ev.conn_index)
  elseif ev.type == "disconnected" then
    print("disconnected conn_index=" .. ev.conn_index .. " reason=" .. ev.reason)
  end
end)

while true do
  ble.process_events(100)
end
```

### Event types and key fields

| `ev.type` | Key fields |
|-----------|-----------|
| `adv_started` | `reason` = `"user_start"` \| `"re_advertise"` |
| `adv_stopped` | `reason` = `"user_stop"` \| `"connection_full"` \| `"complete"` \| `"deinit"` \| `"error"`; `error_code` (only on `"error"`) |
| `connected` | `conn_index`, `conn_handle` (diagnostic), `peer_addr`, `peer_addr_type` |
| `disconnected` | `conn_index`, `reason` = `"local"` \| `"remote"` \| `"timeout"` \| `"deinit"` \| `"error"` |
| `mtu_changed` | `conn_index`, `mtu` |
| `security_changed` | `conn_index`, `encrypted`, `authenticated`, `bonded`, `key_size`, `reason` |
| `gatts_read` | `conn_index`, `service_id`, `characteristic_id`, `uuid_service`, `uuid_characteristic`, `uuid_descriptor`, `offset` |
| `gatts_write` | `conn_index`, `service_id`, `characteristic_id`, `uuid_service`, `uuid_characteristic`, `uuid_descriptor`, `data`, `offset` |
| `subscribe_changed` | `conn_index`, `service_id`, `characteristic_id`, `uuid_service`, `uuid_characteristic`, `notify`, `indicate` |
| `notify_complete` | `conn_index`, `characteristic_id`, `status`, `error_code` |
| `indicate_complete` | `conn_index`, `characteristic_id`, `status`, `error_code` |

---

## SMP / Security

Bonding enabled, Secure Connections enabled, MITM disabled (Just Works), IO
capability no-input/no-output. Bond persistence requires
`CONFIG_BT_NIMBLE_NVS_PERSIST=y` in sdkconfig; without it bonds are lost on
reboot.

Use `ble.smp_config()` before `ble.init()` when authenticated GATT permissions
or a non-default repeat-pairing policy are needed. Repeat pairing supports
`"ignore"` and `"delete_retry"`; the default is `"delete_retry"`. Phase 1 does
not expose passkey / numeric comparison callbacks or bond-management Lua APIs.

---

## Error Codes

| Code | Meaning |
|------|---------|
| `ble_init_not_initialized` | `ble.init()` not called yet |
| `ble_invalid_state` | Operation not valid in current state |
| `ble_unsupported` | Feature not supported in this phase |
| `ble_resource_busy` | NimBLE returned a busy / unexpected error |
| `ble_conn_invalid_index` | `conn_index` outside 0..2 |
| `ble_conn_index_required` | Multiple connections, caller must supply `conn_index` |
| `ble_conn_not_found` | No connection at the given slot |
| `ble_mtu_invalid` | MTU outside 23..517 |
| `ble_adv_busy` | Advertising already active |
| `ble_adv_data_too_long` | AD payload exceeds 31-byte limit |
| `ble_gatt_already_started` | GATT profile already active |
| `ble_gatt_char_not_found` | Characteristic lookup failed |
| `ble_gatt_char_ambiguous` | UUID lookup matched multiple characteristics |
| `ble_gatt_not_subscribed` | Peer has not subscribed to notify / indicate |
| `ble_gatt_data_too_long` | Characteristic or ATT payload exceeds the allowed length |
| `ble_gatt_send_failed` | NimBLE failed to send notify / indicate |
| `ble_gatt_indicate_in_progress` | A connection already has a pending indication |
| `ble_smp_invalid_config` | Invalid SMP config or authenticated GATT permission without MITM-capable SMP |
| `ble_smp_security_failed` | Secure notify / indicate attempted before security requirements were met |
| `ble_smp_nvs_persist_disabled` | `require_bond_persist = true` but NimBLE NVS persistence is disabled |
