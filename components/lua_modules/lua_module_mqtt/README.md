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
- Call `client:poll(timeout_ms)` to read one received message
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

### Receive model
Incoming messages are copied onto an internal queue and read with `poll`. This
keeps callbacks off the Lua state, which is single-threaded. Messages larger
than the esp-mqtt input buffer arrive fragmented and are dropped; raise the
broker buffer or keep payloads small.

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
