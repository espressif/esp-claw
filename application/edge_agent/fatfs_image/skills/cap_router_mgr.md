# Router Rule Management

Use this skill to inspect and modify event router rules through the direct callable capabilities in `cap_router_mgr`.

## When to use
- The task is to list, inspect, add, update, delete, or reload router rules.
- The task is to change router behavior by capability calls, not by editing rule files directly.
- The task requires deciding between `add_router_rule` and `update_router_rule` for one rule id.

## Operating rules
- Use direct router manager capabilities, not console wrappers.
- Prefer changing exactly one rule per turn.
- Treat `rule_json` as a JSON string, not an embedded object.
- After `add_router_rule` or `update_router_rule`, always call `get_router_rule`.
- Use `reload_router_rules` only when the task explicitly requires disk reload semantics.

## Capability contract
- `list_router_rules`
  Input: `{}`
  Output: JSON array of rule objects.
- `get_router_rule`
  Input: `{"id":"rule_id"}`
  Output: one rule object, or standard error JSON with `ok=false`.
- `add_router_rule`
  Input: `{"rule_json":"{\"id\":\"rule_id\",\"match\":{\"event_type\":\"message\"},\"actions\":[{\"type\":\"drop\"}]}"}`.
  Output: `{"action":"add_router_rule","ok":true,"id":"rule_id"}`.
  If the error says `rule id already exists; use update_router_rule`, switch to `update_router_rule`.
- `update_router_rule`
  Input: same shape as `add_router_rule`.
  Output: `{"action":"update_router_rule","ok":true,"id":"rule_id"}`.
  If the error says `rule id not found; use add_router_rule`, switch to `add_router_rule`.
- `delete_router_rule`
  Input: `{"id":"rule_id"}`
  Output: `{"action":"delete_router_rule","ok":true,"id":"rule_id"}`
- `reload_router_rules`
  Input: `{}`
  Output: `{"action":"reload_router_rules","ok":true}`

## Standard workflow
1. Call `list_router_rules`.
2. Check whether the target `id` already exists.
3. Build one complete replacement rule object.
4. Call `add_router_rule` for a new id or `update_router_rule` for an existing id.
5. Call `get_router_rule` and verify the stored shape.

## Rule contract
- Required fields in decoded `rule_json`:
  - `id`
  - `match`
  - `actions`
  - `match.event_type`
- Common optional rule fields:
  - `description`
  - `enabled`
  - `consume_on_match`
  - `ack`
  - `vars`
- Supported `match` fields:
  - `event_type`
  - `event_key`
  - `source_cap`
  - `source_channel`
  - `chat_id`
  - `content_type`
  - `text`

## Match semantics
- `match.event_type` is required; all other match fields are optional exact-match filters.
- `event_type`, `event_key`, `source_cap`, and `source_channel` values are not fixed enums in this skill; they must match values emitted elsewhere in the system.
- Use `match.source_channel` as the canonical rule field. The runtime also accepts `channel` as an alias.

## Actions
- Supported `type`:
  - `call_cap`
  - `run_agent`
  - `run_script`
  - `send_message`
  - `emit_event`
  - `drop`
- Optional action-level fields:
  - `caller`: `system` | `agent` | `console`
  - `capture_output`
  - `fail_open`
- Requirements and defaults:
  - `call_cap`: requires `cap` and `input`.
  - `run_agent`: `input` optional; defaults to `{}`. `text` falls back to event text. `target_channel` and `target_chat_id` fall back to the current event route.
  - `run_script`: requires `input`; common fields are `path`, `args`, `async`.
  - `send_message`: requires `input`; common fields are `channel`, `chat_id`, `message`. Missing `channel` falls back to target/source channel. Missing `chat_id` falls back to target endpoint/event chat id. Missing `message` falls back to previous action output.
  - `emit_event`: requires `input`; common fields are `event_type`, `source_cap`, `source_channel`, `chat_id`, `message_id`, `content_type`, `text`, `payload_json`, `session_policy`. Defaults: `source_cap=claw_event_router`, `event_type=trigger`, `content_type=trigger`, `payload_json={}`, invalid or missing `session_policy=trigger`.
  - `drop`: no special input.

## Template context
- Action inputs are rendered against the current event context before execution.
- Common fields:
  - `{{event.event_type}}`
  - `{{event.event_key}}`
  - `{{event.source_cap}}`
  - `{{event.channel}}`
  - `{{event.source_channel}}`
  - `{{event.target_channel}}`
  - `{{event.chat_id}}`
  - `{{event.message_id}}`
  - `{{event.content_type}}`
  - `{{event.session_policy}}`
  - `{{event.text}}`
- Parsed payload fields are available under `event.payload`, for example `{{event.payload.kind}}`.

## Decision policy
- If creating a new rule and the id already exists, do not call `add_router_rule`.
- If modifying an existing rule, inspect it first and preserve fields not meant to change.
- If it is unclear whether to create or replace, inspect first with `list_router_rules` or `get_router_rule`.
- Do not use `reload_router_rules` as a substitute for add or update.

## Common failure causes
- Putting `event_type` at the rule top level instead of `match.event_type`.
- Passing `rule_json` as an object instead of a string.
- Omitting `id` or `actions`, or using an empty `actions` array.
- Using an unsupported action `type`.
- Mixing `match.source_channel` with action input `channel`.

## Canonical examples

### New session on `/new`

```json
{
  "rule_json": "{\"id\":\"im_new_session\",\"enabled\":true,\"consume_on_match\":true,\"match\":{\"event_type\":\"message\",\"event_key\":\"text\",\"content_type\":\"text\",\"text\":\"/new\"},\"actions\":[{\"type\":\"call_cap\",\"cap\":\"roll_chat_session\",\"input\":{}},{\"type\":\"send_message\",\"input\":{\"channel\":\"{{event.source_channel}}\",\"chat_id\":\"{{event.chat_id}}\",\"message\":\"Started a new session.\"}}]}"
}
```

### Route IM text to agent

```json
{
  "rule_json": "{\"id\":\"im_any_message_agent\",\"enabled\":true,\"consume_on_match\":true,\"match\":{\"event_type\":\"message\",\"event_key\":\"text\",\"content_type\":\"text\"},\"actions\":[{\"type\":\"run_agent\",\"input\":{\"target_channel\":\"{{event.source_channel}}\",\"session_policy\":\"chat\"}}]}"
}
```
