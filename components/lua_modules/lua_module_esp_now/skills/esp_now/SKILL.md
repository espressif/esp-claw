---
{
  "name": "esp_now",
  "description": "Use ESP-NOW on ESP-Claw: send or broadcast short messages directly to nearby Espressif devices by MAC address, receive frames, manage peers, and process ESP-NOW events. Connectionless, no WiFi AP join needed.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# ESP-NOW Skill (lua_module_esp_now)

This skill describes how an Agent controls ESP-NOW on ESP-Claw via Lua scripts.
The module is a thin adapter over the ESP-IDF ESP-NOW API and works on
ESP-IDF 5.5.x and 6.x.

ESP-NOW is a connectionless protocol for sending short payloads directly to
another Espressif device by its WiFi MAC address, or broadcasting to all
listeners on the same WiFi channel. It does not require the devices to join the
same access point.

## Capabilities

- Initialize / deinitialize ESP-NOW (`esp_now.init` / `esp_now.deinit`).
- Send a unicast or broadcast frame (`esp_now.send`).
- Manage peers (`add_peer`, `mod_peer`, `del_peer`, `get_peer`, `peer_exists`,
  `get_peer_num`, `fetch_peers`).
- Optional encryption (`set_pmk`, per-peer `lmk`) and low-power wake window
  (`set_wake_window`).
- Read/align the WiFi channel and read the interface MAC (`get_channel`,
  `set_channel`, `get_mac`) so a standalone peer board can be matched.
- Unified event queue: `esp_now.on_event(fn)` + `esp_now.process_events(ms)` for
  received frames and send-complete status.
- Diagnostics via `esp_now.stats()`.

## Core Rules

- WiFi is already initialized and started by the firmware at boot, so
  `esp_now.init()` works without extra WiFi setup. If WiFi is somehow down,
  `esp_now.init()` returns `nil, "espnow_invalid_state"`.
- A peer (including `esp_now.BROADCAST_MAC`) must be added with
  `esp_now.add_peer{...}` before `esp_now.send(...)`, otherwise send returns
  `nil, "espnow_peer_not_found"`.
- Peers must share the same WiFi channel. When connected to an AP the STA
  channel is fixed; add peers with `channel = 0` to use the current channel.
  Read the current channel with `esp_now.get_channel()` and align a standalone
  board with `esp_now.set_channel(ch)` (a manual channel may not persist while
  STA-connected to an AP).
- MAC addresses and payloads are raw byte strings, not hex text. A MAC is
  exactly 6 bytes.
- If a script fails, report the returned error code to the user directly; do not
  retry blindly with changed arguments.
- If sends never succeed or no frames arrive, the likely cause is a WiFi channel
  mismatch. Tell the user both devices must share the same 2.4 GHz channel, report
  `esp_now.get_channel()`, and note this device follows its AP's channel. Leave the
  fix to the user; do not auto-change channels or add retry loops.

## Quick-start entry points

Listen for incoming ESP-NOW frames and print them (also prints this device MAC
so a peer can target it):

```text
lua --run --path builtin/skills/esp_now/scripts/start_espnow_listener.lua
```

Broadcast a one-off message to all listeners on the current channel:

```text
lua --run --path builtin/skills/esp_now/scripts/broadcast_espnow.lua
```

Edit the `CONFIG` table at the top of each script before running.

## API summary

See the module document (`esp_now.md`) for full signatures. All fallible calls
return `true` / a value on success, or `nil, "error_code"` on recoverable
failure; type errors raise a Lua error.

- `esp_now.init()`, `esp_now.deinit()`, `esp_now.get_version()`
- `esp_now.send(peer_mac, data)`
- `esp_now.add_peer{ peer_addr=, channel=, ifidx=, encrypt=, lmk= }`
- `esp_now.mod_peer{...}`, `esp_now.del_peer(mac)`, `esp_now.get_peer(mac)`
- `esp_now.peer_exists(mac)`, `esp_now.get_peer_num()`, `esp_now.fetch_peers()`
- `esp_now.set_pmk(pmk16)`, `esp_now.set_wake_window(window)`
- `esp_now.set_channel(channel)`, `esp_now.get_channel()`, `esp_now.get_mac([ifidx])`
- `esp_now.on_event(fn)`, `esp_now.process_events(timeout_ms)`, `esp_now.stats()`

## Constants

- `esp_now.BROADCAST_MAC` : 6-byte broadcast MAC.
- `esp_now.MAX_DATA_LEN` : maximum `send` payload length.
- `esp_now.IFIDX` : `{ STA, AP }`.
- `esp_now.SEND_STATUS` : `{ SUCCESS = 0, FAIL = 1 }`.

## Events

Register one callback with `esp_now.on_event(fn)` and poll with
`esp_now.process_events(timeout_ms)`:

| `ev.type` | Key fields |
|-----------|-----------|
| `recv` | `src_mac`, `dst_mac`, `data`, `rssi`, `channel` |
| `send` | `peer_mac`, `status`, `success` |

```lua
local esp_now = require("esp_now")
esp_now.on_event(function(ev)
    if ev.type == "recv" then
        print("recv from", ev.src_mac, ev.data)
    elseif ev.type == "send" then
        print("send success", ev.success)
    end
end)
while true do
    esp_now.process_events(100)
end
```

## Error Codes

| Code | Meaning |
|------|---------|
| `espnow_not_init` | Call `esp_now.init()` first |
| `espnow_invalid_state` | WiFi stack is not initialized/started |
| `espnow_invalid_arg` | Invalid argument passed to ESP-NOW |
| `espnow_no_mem` | Out of memory |
| `espnow_peer_list_full` | Peer list is full |
| `espnow_peer_not_found` | Peer not added before use |
| `espnow_peer_exists` | Peer already added |
| `espnow_interface_mismatch` | Peer interface does not match WiFi mode |
| `espnow_channel_mismatch` | Peer channel does not match the current channel |
| `espnow_internal` | ESP-NOW internal error |
