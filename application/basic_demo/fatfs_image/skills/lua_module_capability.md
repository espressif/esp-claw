# Lua Capability

This skill describes how to call registered capabilities directly from Lua.

## How to call
- Import it with `local capability = require("capability")`
- Main API: `ok, out, err = capability.call(name, payload[, opts])`. ALWAYS follow the 3-element pattern, `ok, out, err`, to receive the result, for example `local ok, xxx_out, err = capability.call("xxx", ...)`.
- `name` must match the real registered capability id, for example `qq_send_message`, `qq_send_image`, or `qq_send_file`.

## API

### `capability.call(name, payload[, opts])`
- Inputs:
  - `name`: required `string`, capability name or id
  - `payload`: optional `nil | table | string`
  - `opts`: optional `table`
- Output:
  - success: `true, output_string, nil`
  - failure: `false, output_string|nil, error_string`

## Payload rules
- `nil` becomes `{}`.
- A Lua `table` is serialized to compact JSON.
- A `string` must already be valid JSON and is passed through unchanged.
- The payload keys must match the target capability schema exactly. For example, `qq_send_message` expects `message`, not `text`.

## Supported `opts`
- `session_id: string`
- `channel: string`
- `chat_id: string`
- `source_cap: string`

If an `opts` field is missing, the module tries to inherit the same field from global `args`.

## IM capability mapping
- QQ text/image/file:
  - `qq_send_message` with required `chat_id` and `message`
  - `qq_send_image` with required `chat_id`, `path`, and optional `caption`
  - `qq_send_file` with required `chat_id`, `path`, and optional `caption`
- Telegram text/image/file:
  - `tg_send_message` with required `chat_id` and `message`
  - `tg_send_image` with required `chat_id`, `path`, and optional `caption`
  - `tg_send_file` with required `chat_id`, `path`, and optional `caption`
- WeChat text/image:
  - `wechat_send_message` with `message`
  - `wechat_send_image` with `path` and optional `caption`
- Always pass `chat_id` explicitly for `qq`, `tg`, and `wechat` calls.
- Prefer putting `chat_id` in `opts` for `qq` and `tg`, and in `payload` for `wechat`.

## If you want to call other generic capabilities
- Use activate_skill to activate the needed capability and learn what is the schema of the input arguments; what is the schema of the return values.
- Then choose the proper input arguments and call the capability with `capability.call`, and use the correct way to parse the results.

## Examples

### Send a QQ message
```lua
local capability = require("capability")

local ok, out, err = capability.call("qq_send_message", {
  message = "hello from lua"
}, {
  channel = "qq",
  chat_id = "c2c:123456",
  session_id = "demo-session",
  source_cap = "lua_script"
})
print(ok, out, err)
```

### Send an image or file
```lua
local capability = require("capability")

local ok1, out1, err1 = capability.call("qq_send_image", {
  path = "/fatfs/statics/ESP-Claw.png",
  caption = "image from lua"
}, {
  channel = "qq",
  chat_id = "c2c:123456",
  source_cap = "lua_script"
})

local ok2, out2, err2 = capability.call("qq_send_file", {
  path = "/fatfs/reports/status.json",
  caption = "latest report"
}, {
  channel = "qq",
  chat_id = "c2c:123456",
  source_cap = "lua_script"
})

print(ok1, out1, err1)
print(ok2, out2, err2)
```

### Send a Telegram message
```lua
local capability = require("capability")

local ok, out, err = capability.call("tg_send_message", {
  message = "hello from lua"
}, {
  channel = "telegram",
  chat_id = "-1001234567890",
  source_cap = "lua_script"
})
print(ok, out, err)
```

### Send a Telegram message to another chat
```lua
local capability = require("capability")

local ok, out, err = capability.call("tg_send_message", {
  message = "telegram reply from lua"
}, {
  channel = "telegram",
  chat_id = "-1009876543210",
  source_cap = "lua_script"
})
print(ok, out, err)
```

### Send a WeChat message
```lua
local capability = require("capability")

local ok, out, err = capability.call("wechat_send_message", {
  chat_id = "room123",
  message = "hello from lua"
}, {
  channel = "wechat",
  source_cap = "lua_script"
})
print(ok, out, err)
```

### Send a WeChat image
```lua
local capability = require("capability")

local ok, out, err = capability.call("wechat_send_image", {
  chat_id = "wxid_abc123",
  path = "/fatfs/statics/ESP-Claw.png",
  caption = "image from lua"
}, {
  channel = "wechat",
  source_cap = "lua_script"
})
print(ok, out, err)
```
