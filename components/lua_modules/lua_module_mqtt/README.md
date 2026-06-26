# Lua MQTT

This module describes how to correctly use `mqtt` when writing Lua scripts.
When a request mentions an `MQTT` broker, publishing, or subscribing to topics,
use this `mqtt` module by default. It wraps the ESP-IDF `esp-mqtt` client.

## How to call
- Import it with `local mqtt = require("mqtt")`
- Call `local client = mqtt.new(uri, opts)` to create a client
- Call `client:connect(timeout_ms)` to start and wait for the broker handshake
- Call `client:publish(topic, payload, qos, retain)` to publish
- Call `client:subscribe(topic, qos)` to subscribe
- Call `client:poll(timeout_ms)` to read one received message (pull model), or
- Register `client:on(topic, fn)` and drain with `client:dispatch()` (callback model)
- Call `client:disconnect()` or `client:close()` when done

### `mqtt.new([uri[, opts]])`
- `uri`: broker URL, e.g. `mqtt://192.168.1.10:1883` or `mqtts://host:8883`
- `opts` (optional table):
  - `username`, `password`: broker credentials
  - `client_id`: MQTT client id
  - `keepalive`: keepalive seconds
  - `rx_queue_len`: received-message buffer length (default `16`, max `128`)

The broker URL, username, password, and client id can be preset in the device
web config (MQTT tab). When `uri` or a credential field is omitted, the saved
value is used; explicit arguments always override the saved defaults. Call
`mqtt.new()` with no arguments to connect to the configured broker. If no `uri`
is given and none is configured, the call errors.

### `client:connect(timeout_ms)`
Starts the client and blocks until connected, up to `timeout_ms`
(default `10000`). Returns `true` on success, `false` on timeout.

### `client:publish(topic, payload, qos, retain)`
`qos` defaults to `0`, `retain` defaults to `false`. Returns the message id.

### `client:subscribe(topic, qos)` / `client:unsubscribe(topic)`
`qos` defaults to `0`. Returns the message id.

### `client:poll(timeout_ms)`
Returns the next received message as `{ topic = ..., payload = ... }`, or `nil`
when none arrives within `timeout_ms` (default `0`, non-blocking). Run a polling
loop inside an async Lua job so incoming messages keep draining.

### `client:on(topic, fn)` / `client:off([topic])` / `client:dispatch()`
Callback model, mirroring other ESP-Claw event modules (e.g. `knob`).

- `on(topic, fn)` registers a Lua function for a topic filter. Re-registering the
  same pattern replaces the previous function. Up to 16 callbacks per client.
- `off(topic)` removes the callback for that exact pattern; `off()` with no
  argument removes all callbacks.
- `dispatch()` drains the receive queue and calls every registered callback whose
  pattern matches each message's topic, passing `{ topic = ..., payload = ... }`.
  A single message may fire several callbacks. Returns the number of invocations.
  Callbacks run on the Lua task that calls `dispatch`, never on the MQTT task.

Run `dispatch()` in a service loop, the same way `poll` is looped:

```lua
client:on("sensors/+/temp", function(msg)
  print(msg.topic, msg.payload)
end)
while true do
  client:dispatch()
  delay.delay_ms(100)
end
```

#### Topic wildcards
`on` matches the message topic against the registered filter using MQTT wildcards:

- `+` matches exactly one level (the text between `/` separators).
  `sensors/+/temp` matches `sensors/cocina/temp`, not `sensors/cocina/x/temp`.
- `#` matches the remaining levels and must be the last filter level. It also
  matches zero levels, so `sensors/#` matches both `sensors/temp` and
  `sensors/cocina/temp`, and `sensors/#` matches `sensors`.
- With no wildcards, the filter must equal the topic exactly
  (`claw/announce` matches only `claw/announce`).

Subscribe with `client:subscribe(filter, qos)` for the broker to deliver those
topics; the same filter string is what you pass to `on`.

### Receive model
Incoming messages are copied onto an internal queue. Read them with **either**
`poll` (pull) **or** `on` + `dispatch` (callbacks) — not both on the same client:
the two share one queue, so each message a `poll` returns is one `dispatch` never
sees, and vice versa. Either way, message handling stays on the Lua state, which
is single-threaded. Messages larger than the esp-mqtt input buffer arrive
fragmented and are dropped; raise the broker buffer or keep payloads small.

## Example
```lua
local mqtt = require("mqtt")

local client = mqtt.new("mqtt://192.168.1.10:1883", {
  username = "esp",
  password = "secret",
  client_id = "esp_claw_1",
})

if client:connect(10000) then
  client:publish("casa/esp32/estado", '{"led":"rojo"}', 1, false)
  client:subscribe("casa/esp32/cmd", 1)

  local msg = client:poll(5000)
  if msg then
    print("rx", msg.topic, msg.payload)
  end
end

client:close()
```
