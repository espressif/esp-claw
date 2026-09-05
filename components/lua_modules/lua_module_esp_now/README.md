# Lua ESP-NOW

`require("esp_now")` wraps the ESP-IDF ESP-NOW API for connectionless,
low-latency peer-to-peer messaging between Espressif devices without joining
the same WiFi access point.

The module is source-compatible with ESP-IDF 5.5.x and 6.x.

## When To Use

- Send short messages directly to another ESP device by its WiFi MAC address.
- Broadcast small payloads to all listening devices on the same WiFi channel.
- Build simple sensor/remote links that do not need TCP/IP.

Use the `ble` module instead for phone/BLE-central interoperability, and normal
sockets (via `http_server` / capabilities) for IP networking.

## Core Rules

- WiFi must already be initialized and started. In this firmware `wifi_manager`
  does this at boot, so `esp_now.init()` succeeds without extra setup.
- Every destination, including the broadcast address, must be added with
  `esp_now.add_peer{...}` before `esp_now.send(...)`.
- ESP-NOW peers must be on the same WiFi channel. When the device is connected
  to an AP, the STA channel is fixed by the AP; add peers with `channel = 0`
  (use current channel) unless you manage channels yourself. Use
  `esp_now.get_channel()` to read the current channel and
  `esp_now.set_channel(ch)` to align a standalone board to it (a manual channel
  may not persist while STA-connected to an AP).
- MAC addresses and payloads are raw Lua strings (not hex text). A MAC is
  exactly 6 bytes.
- Receive and send-complete notifications are delivered through an event queue.
  Register a callback with `esp_now.on_event(fn)` and drain it in a loop with
  `esp_now.process_events(timeout_ms)`.
- Call `esp_now.deinit()` when finished to release the ESP-NOW driver.

## How To Call

- `esp_now.init()` -> `true` | `nil, err` : start ESP-NOW and register callbacks.
- `esp_now.deinit()` -> `true` : stop ESP-NOW and clear queued events.
- `esp_now.get_version()` -> `integer` | `nil, err` : ESP-NOW protocol version.
- `esp_now.send(peer_mac, data)` -> `true` | `nil, err` : queue a frame to a peer.
- `esp_now.add_peer(opts)` / `esp_now.mod_peer(opts)` -> `true` | `nil, err`.
- `esp_now.del_peer(peer_mac)` -> `true` | `nil, err`.
- `esp_now.get_peer(peer_mac)` -> `peer_table` | `nil, err`.
- `esp_now.peer_exists(peer_mac)` -> `boolean`.
- `esp_now.get_peer_num()` -> `{ total, encrypt }` | `nil, err`.
- `esp_now.fetch_peers()` -> `array of peer_table`.
- `esp_now.set_pmk(pmk)` -> `true` | `nil, err` : 16-byte primary master key.
- `esp_now.set_wake_window(window)` -> `true` | `nil, err` : 0..65535.
- `esp_now.set_channel(channel)` -> `true` | `nil, err` : lock the radio to a
  primary channel (1..14) so a remote peer can be aligned to it.
- `esp_now.get_channel()` -> `integer` | `nil, err` : current primary channel.
- `esp_now.get_mac([ifidx])` -> `6-byte string` | `nil, err` : interface MAC
  (defaults to `esp_now.IFIDX.STA`) that a peer can target.
- `esp_now.on_event(fn_or_nil)` -> `true` : set or clear the event callback.
- `esp_now.process_events(timeout_ms)` -> `integer` : events dispatched.
- `esp_now.stats()` -> table with runtime counters.

### `add_peer` / `mod_peer` options

| Field       | Type            | Default            | Notes                                   |
|-------------|-----------------|--------------------|-----------------------------------------|
| `peer_addr` | 6-byte string   | required           | Peer WiFi MAC.                          |
| `channel`   | integer 0..14   | `0`                | `0` = current channel.                  |
| `ifidx`     | integer         | `esp_now.IFIDX.STA`| `IFIDX.STA` or `IFIDX.AP`.              |
| `encrypt`   | boolean         | `false`            | Auto-enabled when `lmk` is set.         |
| `lmk`       | 16-byte string  | none               | Local master key for encrypted peer.    |

### Constants

- `esp_now.BROADCAST_MAC` : 6-byte broadcast address (`FF:FF:FF:FF:FF:FF`).
- `esp_now.MAX_DATA_LEN` : maximum payload length for `send`.
- `esp_now.IFIDX` : `{ STA, AP }`.
- `esp_now.SEND_STATUS` : `{ SUCCESS = 0, FAIL = 1 }`.

## Events

`esp_now.on_event(fn)` receives one table per event:

- `recv` : `{ type = "recv", src_mac, dst_mac, data, rssi, channel }`.
- `send` : `{ type = "send", peer_mac, status, success }` where `status` is
  `esp_now.SEND_STATUS.SUCCESS` or `.FAIL` and `success` is a boolean.

## Minimal Broadcast Example

```lua
local esp_now = require("esp_now")

assert(esp_now.init())
assert(esp_now.add_peer({ peer_addr = esp_now.BROADCAST_MAC }))

esp_now.on_event(function(ev)
    if ev.type == "recv" then
        print("from", ev.src_mac, "rssi", ev.rssi, "data", ev.data)
    elseif ev.type == "send" then
        print("send success:", ev.success)
    end
end)

assert(esp_now.send(esp_now.BROADCAST_MAC, "hello"))

for _ = 1, 50 do
    esp_now.process_events(100)
end

esp_now.deinit()
```

## Common Errors

- `espnow_not_init` : call `esp_now.init()` first.
- `espnow_peer_not_found` : add the peer before sending.
- `espnow_peer_list_full` : too many peers; delete unused ones.
- `espnow_channel_mismatch` : peer is on a different channel. Both devices must
  share the same 2.4 GHz channel; while connected to an AP this device follows
  the AP's channel (see `esp_now.get_channel()`).
- `espnow_invalid_state` : WiFi is not initialized/started.
