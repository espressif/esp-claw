# Lua Event Publisher

This module describes how to publish events from Lua scripts.

## API

- Import it with `local event_publisher = require("event_publisher")`.
- Use dot syntax: `event_publisher.publish_message(...)`, not `event_publisher:publish_message(...)`.
- All APIs return `true` on success and raise a Lua error on invalid arguments or publish failure.

### `event_publisher.publish_message(opts)`

Publishes an IM-style text message event.

Required fields:
- `source_cap`: string
- `channel`: string
- `chat_id`: string
- `text`: string

Optional fields:
- `sender_id`: string
- `message_id`: string

### `event_publisher.publish_trigger(opts)`

Publishes a trigger event.

Required fields:
- `source_cap`: string
- `event_type`: string
- `event_key`: string

Payload fields:
- `payload_json`: optional JSON string
- `payload`: optional Lua table serialized to JSON

If neither `payload_json` nor `payload` is provided, the payload defaults to `{}`.

### `event_publisher.publish(opts)`

Publishes a general event.

Required fields:
- `source_cap`: string
- `event_type`: string

Optional fields:
- `event_id`: string; defaults to a generated `lua-...` id
- `source_channel`: string
- `target_channel`: string
- `source_endpoint`: string
- `target_endpoint`: string
- `chat_id`: string
- `sender_id`: string
- `message_id`: string
- `correlation_id`: string
- `content_type`: string
- `text`: string
- `timestamp_ms`: integer; defaults to current uptime milliseconds
- `session_policy`: `"chat"`, `"trigger"`, `"global"`, `"ephemeral"`, or `"nosave"`
- `payload_json`: JSON string
- `payload`: Lua table serialized to JSON

When `session_policy` is omitted and `event_type == "trigger"`, the session policy defaults to `"trigger"`.

## Example: message

```lua
local event_publisher = require("event_publisher")

event_publisher.publish_message({
  source_cap = "lua_script",
  channel = args.channel,
  chat_id = args.chat_id,
  text = "Button pressed!",
})
```

## Example: trigger

```lua
local event_publisher = require("event_publisher")

event_publisher.publish_trigger({
  source_cap = "lua_script",
  event_type = "trigger",
  event_key = "button.single_click",
  payload = {
    gpio = 0,
  },
})
```
