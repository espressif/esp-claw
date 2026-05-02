# Lua Event Publisher

This skill describes how to correctly use `event_publisher` when writing Lua scripts.

## Default (use this first — avoids runtime errors)

From **callbacks** (button, timers) or any code path where brevity matters, send IM text with a **single string**. The firmware sets `source_cap = "lua_script"` and fills `channel` / `chat_id` from global `args` when the agent started the script in a chat:

```lua
local ep = require("event_publisher")
ep.publish_message("Button pressed!")
```

**Dot syntax only:** `ep.publish_message(...)`. Do **not** use `ep:publish_message(...)` (colon passes the wrong first argument).

Do **not** write a **partial table** (e.g. only `channel`, `chat_id`, `text`). That fails with `missing required field 'source_cap'`. Either use the **string** form above, or a **complete** table (next section).

## Table form (only when you need extra fields)

If you pass a **table**, **every** of these is required unless noted:

| Field | Required? | Notes |
|-------|-----------|--------|
| `source_cap` | **Yes** | e.g. `"lua_script"` |
| `text` | **Yes** | message body |
| `channel` | If not in `args` | Often use `args.channel` when agent-injected |
| `chat_id` | If not in `args` | Often use `args.chat_id` when agent-injected |

```lua
local ep = require("event_publisher")
ep.publish_message({
  source_cap = "lua_script",
  channel = args.channel,
  chat_id = args.chat_id,
  text = "Button pressed!",
})
```

## Session context (agent / IM)

When the agent runs your script from a chat session, the firmware may merge `channel`, `chat_id`, and `session_id` into the global `args` table. Prefer **string** `publish_message` so you do not forget `source_cap` in callbacks.

If `args.channel` / `args.chat_id` are missing (e.g. CLI run without IM), pass `channel` and `chat_id` explicitly in the **table** (still include `source_cap` and `text`).

## Other APIs

- `publish_trigger` and `publish` always take a **table** as the first argument (see their fields in docs when used).

## Example (CLI / no `args`)

```lua
local event_publisher = require("event_publisher")

event_publisher.publish_message({
  source_cap = "lua_script",
  channel = "custom",
  chat_id = "demo",
  text = "hello"
})
```
