---
{
  "name": "scheduled_task",
  "description": "Add timer-based scheduled tasks, periodic reminders, scheduled agent wake-ups, fixed IM messages, or scheduled Lua script runs. Prefer to use this skill rather than calling scheduler_add and add_router_rule directly.",
  "metadata": {
    "cap_groups": [
      "cap_lua",
      "cap_scheduler",
      "cap_router_mgr"
    ],
    "manage_mode": "readonly"
  }
}
---

# Scheduled Task

Use this skill when the user asks to add a scheduled task, timer, periodic reminder, timed agent wake-up, fixed IM reminder, or scheduled Lua script run.

Run exactly one bundled Lua script with `lua_run_script`:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{},"timeout_ms":60000}
```

If script execution returns an error, report that error directly to the user.
Do not retry with changed arguments or run another script in the same turn unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "task_id": {
      "type": "string",
      "description": "Stable schedule id. Use lowercase letters, digits, underscore, or hyphen."
    },
    "kind": {
      "type": "string",
      "enum": ["cron", "interval"]
    },
    "cron_expr": {
      "type": "string",
      "description": "Required when kind is cron. Use a 5-field cron expression: minute hour mday month wday."
    },
    "interval_ms": {
      "type": "integer",
      "description": "Required when kind is interval. Must be greater than 0."
    },
    "mode": {
      "type": "string",
      "enum": ["wake_agent", "send_message", "run_script"]
    },
    "text": {
      "type": "string",
      "description": "Required for wake_agent and send_message. For run_script it is optional schedule text."
    },
    "script_path": {
      "type": "string",
      "description": "Required for run_script. Must be an absolute .lua path and must not contain .."
    },
    "script_args": {
      "type": "object",
      "default": {}
    },
    "chat_channel": {
      "type": "string",
      "description": "Required for wake_agent and send_message, for example feishu, weixin, qq, or telegram."
    },
    "chat_id": {
      "type": "string",
      "description": "Required for wake_agent and send_message."
    },
    "enabled": {
      "type": "boolean",
      "default": true
    },
    "trigger_count": {
      "type": "integer",
      "default": 0,
      "description": "Maximum number of times to trigger. Use 0 for unlimited. Preferred over max_runs."
    },
    "max_runs": {
      "type": "integer",
      "default": 0,
      "description": "Compatibility alias for trigger_count. Use trigger_count for new calls."
    }
  },
  "required": ["task_id", "kind", "mode"]
}
```

## Tool Call Inputs

Send a fixed IM message every day at 17:06:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"drink_water_reminder","kind":"cron","cron_expr":"6 17 * * *","mode":"send_message","text":"该喝水了","chat_channel":"feishu","chat_id":"ou_xxx","trigger_count":0},"timeout_ms":60000}
```

Wake the agent every day at 17:09:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"weather_outfit_reminder","kind":"cron","cron_expr":"9 17 * * *","mode":"wake_agent","text":"查询今天的天气，然后根据天气情况告诉我应该穿什么衣服。","chat_channel":"feishu","chat_id":"ou_xxx","trigger_count":0},"timeout_ms":60000}
```

Run a Lua script every 120 seconds:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"hello_world_timer","kind":"interval","interval_ms":120000,"mode":"run_script","text":"hello world timer","script_path":"/fatfs/scripts/hello_world.lua","script_args":{},"trigger_count":3},"timeout_ms":60000}
```

## Behavior

- `wake_agent` creates only one scheduler entry. It emits a `message`/`text` event from the target IM channel and chat, so the existing `im_any_message_agent` router rule handles the agent run and normal chat session context.
- `send_message` and `run_script` create one scheduler entry and one router rule.
- For `send_message` and `run_script`, the skill script uses `add_router_rule` first and `scheduler_add` second through Lua `capability.call`.
- For `wake_agent`, the skill script uses only `scheduler_add` through Lua `capability.call`.
- It does not update existing rules. If an id already exists, the capability error is reported directly.
- If router rule creation succeeds but scheduler creation fails, the script best-effort deletes the newly added router rule and reports both the scheduler error and rollback result.
- `wake_agent` sets scheduler `event_type: "message"`, `event_key: "text"`, `source_channel` from `chat_channel`, `chat_id` from `chat_id`, `content_type: "text"`, and `session_policy: "chat"`.
- `send_message` and `run_script` use `session_policy: "trigger"` in the scheduler entry.
- `trigger_count` maps to scheduler `max_runs`; `0` means unlimited. If both `trigger_count` and `max_runs` are provided, `trigger_count` wins.

## User Reporting

Before running the script, tell the user the chosen execution strategy in concise terms:

- schedule kind and timing, such as the cron expression or interval in milliseconds
- mode: wake agent, send fixed IM message, or run Lua script. Provide more understandable and detailed explanation for the user, such as "the agent will be woken up to do something" instead of "wake agent".
- target IM channel/chat when applicable
- script path when running a Lua script
- trigger count, where `0` means unlimited
- whether a router rule will be created, or whether `wake_agent` will reuse `im_any_message_agent`

After the script succeeds, summarize what was added: `task_id`, mode, schedule kind, trigger count, scheduler id, and router rule id or reused router rule. If the script fails, report the script error directly.

## Recommended Flow

1. Choose a stable `task_id` in lowercase snake_case or kebab-case.
2. Choose `kind` as `cron` for wall-clock schedules or `interval` for relative periodic schedules.
3. Choose exactly one `mode`: `wake_agent`, `send_message`, or `run_script`.
4. Fill the mode-specific required arguments and resolve `trigger_count`.
5. Tell the user the execution strategy and trigger count before making changes.
6. Run `{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua` with `lua_run_script` and `timeout_ms: 60000`.
7. Report the script result or error directly to the user, including the created scheduler and router behavior.
