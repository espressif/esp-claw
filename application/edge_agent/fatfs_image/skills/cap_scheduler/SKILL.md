---
{
  "name": "cap_scheduler",
  "description": "Manage scheduler rules: list/get/add/update/remove/enable/disable/pause/resume/trigger/reload.",
  "metadata": {
    "cap_groups": [
      "cap_scheduler"
    ],
    "manage_mode": "readonly"
  }
}
---

# Scheduler Management

Use this skill when the user needs to inspect or control timer-based schedule rules.

## When to use
- The user asks to list current schedules.
- The user asks to add, update, or remove a schedule.
- The user asks to pause, resume, enable, disable, trigger, or reload a schedule.
- The user asks for a timer-based reminder, periodic check, timed automation, or scheduled agent wake-up.

## Available capabilities
- `scheduler_list`: list all scheduler entries and runtime state.
- `scheduler_get`: get one scheduler entry by `id`.
- `scheduler_add`: add one scheduler entry from `schedule_json` string.
- `scheduler_update`: update one scheduler entry from `schedule_json` string.
- `scheduler_remove`: remove one scheduler entry by `id`.
- `scheduler_enable`: enable one scheduler entry by `id`.
- `scheduler_disable`: disable one scheduler entry by `id`.
- `scheduler_pause`: pause one scheduler entry by `id`.
- `scheduler_resume`: resume one scheduler entry by `id`.
- `scheduler_trigger_now`: trigger one scheduler entry immediately.
- `scheduler_reload`: reload scheduler definitions from disk.

## Calling rules

- `scheduler_add` and `scheduler_update` input must be:

```json
{
  "schedule_json": "<JSON string of one scheduler entry object>"
}
```

- `schedule_json` is a JSON string, not an object.
- Use a stable, unique `id`. `scheduler_add` fails if the id already exists. `scheduler_update` requires the id to already exist.
- `scheduler_get`, `scheduler_remove`, `scheduler_enable`, `scheduler_disable`, `scheduler_pause`, `scheduler_resume`, and `scheduler_trigger_now` take:

```json
{
  "id": "schedule_id"
}
```

## `schedule_json` object fields
- Required fields:
  - `id`: schedule unique id.
  - `kind`: one of `once`, `interval`, or `cron`.
- `enabled`: whether the schedule is active. Defaults to `true`.
- `start_at_ms`: absolute Unix epoch timestamp in milliseconds. Required for `once`. Optional for `interval`.
- `end_at_ms`: optional stop time in Unix epoch milliseconds for `once` or `interval`.
- `interval_ms`: interval period in milliseconds. Required for `interval`.
- `cron_expr`: 5-field cron expression for `cron`, in the form `minute hour mday month wday`.
- `event_type`: event type published to the router. Defaults to `schedule`.
- `event_key`: logical key for router matching and tracing. Defaults to `id`.
- `source_channel`: event source channel. Defaults to `time`.
- `chat_id`: optional target chat id when downstream router actions need one.
- `content_type`: event content type. Defaults to `trigger`.
- `session_policy`: one of `trigger`, `chat`, `global`, `ephemeral`, `nosave`. Defaults to `trigger`.
- `text`: optional event text payload. For agent wake-up rules, use this as the instruction text.
- `payload_json`: optional structured payload. Defaults to `{}`. It may be written either as a JSON string or as a JSON object in the item itself.
- `max_runs`: max trigger count. `0` means unlimited.

## Time semantics
- Use `once` for one-shot execution at a specific absolute time.
- Use `interval` for relative periodic execution such as "every 10 seconds" or "every 5 minutes".
- An `interval` schedule without `start_at_ms` and `end_at_ms` can run before wall-clock time is synchronized.
- Use `cron` for wall-clock aligned repeated execution such as "every day at 08:00" or "every Monday".
- `cron` depends on valid local time and uses the device's current local timezone.
- Supported cron field forms are only `*`, `*/N`, or one explicit number in range. More complex cron syntax such as ranges, lists, or names is not supported.
- Only 5 cron fields are supported. Do not pass a 6-field expression with seconds.

## Runtime behavior
- Scheduler entries only publish events. They do not define post-trigger behavior by themselves.
- At trigger time the scheduler builds an event payload containing:
  - `schedule_id`
  - `planned_time_ms`
  - `fire_time_ms`
  - `kind`
  - `run_count`
  - `user_payload`
- If a schedule is late by more than one scheduler tick, the runtime counts it as missed and advances to the next fire time instead of replaying all missed runs.
- `scheduler_trigger_now` publishes the event immediately and then refreshes future runtime state. It does not delete the schedule definition.

## Integration note
- Scheduler rules only define when an event is published.
- Any post-trigger behavior such as sending a message, running the agent, or calling another capability must be configured elsewhere.
- Router rule are introduced in `cap_router_mgr` skill, not in this skill.

## Recommended workflow
1. Use `scheduler_list` or `scheduler_get` to inspect current state and avoid id conflicts.
2. Choose the time kind: `once`, `interval`, or `cron`.
3. Add or update one scheduler entry with a stable `event_key`.
4. Active `cap_router_mgr` skill and write router rules.

## Common failure causes
- Passing `schedule_json` as an object instead of a string.
- Omitting required fields for the chosen kind, such as missing `start_at_ms`, `interval_ms`, or `cron_expr`.
- Using a 6-field cron expression or unsupported cron syntax.
- Assuming the scheduler itself will send messages or run follow-up actions.

## Examples

### `interval`

Trigger every 30 seconds, 3 times in total.

```json
{
  "schedule_json": "{\"id\":\"drink_reminder_30s\",\"enabled\":true,\"kind\":\"interval\",\"interval_ms\":30000,\"event_type\":\"schedule\",\"event_key\":\"drink_reminder\",\"source_channel\":\"time\",\"chat_id\":\"group:example\",\"content_type\":\"trigger\",\"session_policy\":\"trigger\",\"text\":\"drink reminder tick\",\"payload_json\":{\"message\":\"time to drink water\"},\"max_runs\":3}"
}
```

### `cron`

Trigger every day at 08:00.

```json
{
  "schedule_json": "{\"id\":\"daily_agent_check\",\"enabled\":true,\"kind\":\"cron\",\"cron_expr\":\"0 8 * * *\",\"event_type\":\"schedule\",\"event_key\":\"daily_agent_check\",\"source_channel\":\"time\",\"chat_id\":\"group:example\",\"content_type\":\"trigger\",\"session_policy\":\"trigger\",\"text\":\"Please check the weather today and tell me what I should prepare for going out.\",\"payload_json\":{},\"max_runs\":0}"
}
```

### `once`

Trigger once at a specific timestamp.

```json
{
  "schedule_json": "{\"id\":\"bootstrap_once\",\"enabled\":true,\"kind\":\"once\",\"start_at_ms\":1893456000000,\"event_type\":\"schedule\",\"event_key\":\"bootstrap_once\",\"source_channel\":\"time\",\"content_type\":\"trigger\",\"session_policy\":\"trigger\",\"text\":\"bootstrap once\",\"payload_json\":{\"task\":\"bootstrap\"},\"max_runs\":1}"
}
```

### Pause a schedule

```json
{
  "id": "sedentary_reminder"
}
```
