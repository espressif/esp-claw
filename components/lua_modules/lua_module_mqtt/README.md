# Lua MQTT

The `mqtt` module provides MQTT 3.1.1 clients for Lua. It accepts `mqtt://`,
`mqtts://`, `ws://`, and `wss://` broker URIs.

```lua
local mqtt = require("mqtt")
```

## Create a client

```lua
local client = assert(mqtt.client(options))
```

`options` is a table with the following fields:

| Field | Type | Description |
| --- | --- | --- |
| `uri` | string | Required broker URI. |
| `client_id` | string | MQTT client identifier. |
| `username` | string | Broker username. |
| `password` | string | Broker password. |
| `protocol` | string | Optional. The only accepted value is `"3.1.1"`. |
| `session` | table | Session options described below. |
| `reconnect` | table | Reconnection options described below. |
| `will` | table | Last Will and Testament options described below. |
| `tls` | table | TLS options described below. |
| `limits` | table | Resource limits described below. |

### Session options

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `clean_start` | boolean | `true` | Start with a clean MQTT session. |
| `keep_alive` | integer | `60` | Keep-alive interval in seconds, from 10 to 3600. |

### Reconnection options

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | boolean | `true` | Enable automatic reconnection. |
| `interval_ms` | integer | `10000` | Reconnection interval, from 100 to 3600000 ms. |

### Last Will options

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `topic` | string | Required | Will topic. |
| `payload` | string | empty | Will payload. Binary strings are accepted. |
| `qos` | integer | `0` | QoS level: 0, 1, or 2. |
| `retain` | boolean | `false` | Set the retained flag. |

### TLS options

TLS options apply to `mqtts://` and `wss://` connections.

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `use_crt_bundle` | boolean | `true` for secure URIs | Verify the broker with the ESP certificate bundle. |
| `ca` | string | none | CA certificate in PEM format. |
| `ca_file` | string | none | Absolute path to a CA certificate file. |
| `client_cert` | string | none | Client certificate in PEM format. |
| `client_cert_file` | string | none | Absolute path to a client certificate file. |
| `client_key` | string | none | Client private key in PEM format. |
| `client_key_file` | string | none | Absolute path to a client private key file. |
| `skip_common_name_check` | boolean | `false` | Skip broker certificate name matching. |
| `insecure` | boolean | `false` | Disable the default certificate bundle and skip certificate name matching. |

The inline and file forms of the same certificate are mutually exclusive.
`client_cert` and `client_key` must be configured together. Certificate paths
must be absolute, must not contain `..`, and each file must not exceed 65536
bytes.

### Resource limits

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `event_queue` | integer | `16` | Pending event capacity, from 4 to 64. |
| `max_message_size` | integer | `16384` | Maximum received payload size, from 256 to 1048576 bytes. |
| `mqtt_buffer_size` | integer | ESP-MQTT default | MQTT buffer size, from 512 to 16384 bytes when set. |
| `outbox_size` | integer | ESP-MQTT default | Outbox limit, from 1024 to 1048576 bytes when set. |

## Client methods

### `client:start()`

Starts the client. Returns `true`, or `nil, error` on failure.

### `client:stop()`

Stops the client. Returns `true`, or `nil, error` on failure.

### `client:disconnect()`

Disconnects from the broker. Returns `true`, or `nil, error` on failure.

### `client:reconnect()`

Requests a reconnection. Returns `true`, or `nil, error` on failure.

### `client:publish(topic, payload[, options])`

Publishes a message and returns its packet identifier, or `nil, error` on
failure. `payload` may contain NUL bytes.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `qos` | integer | `0` | QoS level: 0, 1, or 2. |
| `retain` | boolean | `false` | Set the retained flag. |
| `enqueue` | boolean | `true` | Enqueue the message instead of publishing immediately. |
| `store` | boolean | `false` | Store an enqueued QoS 0 message in the outbox. |

### `client:subscribe(filter[, options])`

Subscribes to one topic filter and returns its packet identifier, or
`nil, error` on failure. `options` may be a QoS integer or a table containing
`qos`. The default QoS is 0.

### `client:unsubscribe(filter)`

Unsubscribes from one topic filter and returns its packet identifier, or
`nil, error` on failure.

### `client:on(event_name, callback)`

Registers one callback for an event and returns the client. Registering another
callback for the same event replaces the previous callback. Pass `nil` as the
callback to remove it.

### `client:process_events([timeout_ms[, max_events]])`

Dispatches pending events and returns the number processed. `timeout_ms`
defaults to 0 and must not be negative. `max_events` defaults to 8 and must be
from 1 to 64.

### `client:run([options])`

Dispatches events until the Lua job receives a stop request, then returns
`true`. `options.poll_ms` defaults to 200 and must be from 10 to 10000 ms.

### `client:state()`

Returns one of `"not_initialized"`, `"not_started"`, `"disconnected"`,
`"connected"`, `"waiting_reconnect"`, or `"unknown"`.

### `client:stats()`

Returns a table containing:

- `events_dropped`
- `messages_dropped`
- `callback_errors`
- `messages_received`
- `outbox_bytes`
- `queued_events`
- `state`

### `client:close()`

Releases the client and returns `true`. Calling it more than once is safe.

## Events

Callbacks are dispatched by `process_events()` or `run()`. Every event table
contains `type`. The following events are available:

| Event | Additional fields |
| --- | --- |
| `before_connect` | None. |
| `connected` | `session_present`; `packet_id` and `reason_code` when non-zero. |
| `disconnected` | `packet_id` and `reason_code` when non-zero. |
| `subscribed` | `granted_qos`; `packet_id` and `reason_code` when non-zero. A `granted_qos` value of `0x80` means rejection. |
| `unsubscribed` | `packet_id` and `reason_code` when non-zero. |
| `published` | `packet_id` and `reason_code` when non-zero. |
| `message` | `topic`, `payload`, `qos`, `retain`, `dup`; `packet_id` when non-zero. |
| `error` | `error_type`, `connect_return_code`, `esp_err`, `tls_error`, `cert_verify_flags`, `socket_errno`. |
| `overflow` | `reason`: `event_queue_full`, `message_too_large`, `invalid_fragment`, or `out_of_memory`. |

## Example

```lua
local mqtt = require("mqtt")

local topic = "esp-claw/example"
local client = assert(mqtt.client({
    uri = "mqtts://broker.emqx.io:8883",
    client_id = "esp-claw-example",
}))

client:on("connected", function()
    assert(client:subscribe(topic, { qos = 1 }))
end)

client:on("subscribed", function()
    assert(client:publish(topic, "hello", { qos = 1 }))
end)

client:on("message", function(event)
    print(event.topic, event.payload)
end)

assert(client:start())
client:run()
client:close()
```

## Included test scripts

- `test/mqtt_basic.lua` runs a publish and subscribe loop against a configured
  broker.
- `test/mqtt_server.lua` runs a TLS, QoS 1, binary payload, and unsubscribe
  test against a real broker.
- `test/onenet_demo.lua` connects to a OneNET custom/passthrough product. Its
  OneNET CA certificate is embedded in the script.

Run the OneNET demo with:

```text
lua --run --path /system/skills/builtin_lua_modules/scripts/builtin/test/onenet_demo.lua --args-json "{\"product_id\":\"<product-id>\",\"device_name\":\"<device-name>\",\"token\":\"<access-token>\"}" --timeout-ms 30000
```
