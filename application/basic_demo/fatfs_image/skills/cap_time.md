# Time Sync

Use this skill when the user needs the current real-world time from the network, especially when local device time may be stale or invalid.

## When to use
- The user asks for the current date, time, weekday, or local time.
- A task depends on fresh real-world time rather than model memory.
- The user asks whether device time is valid or wants the device clock synced now.

## Available capability
- `get_current_time`: fetch current network time, update the device clock, and return formatted local time.

## Current scope
- The callable capability exposed to the agent is `get_current_time`.
- It takes an empty input object.
- It returns formatted plain text such as local date, time, timezone, and weekday.

## Calling rules
- Call `get_current_time` directly when the user needs real current time.
- Input should be an empty object:

```json
{}
```

- Prefer this capability over guessing from model knowledge when the exact current time matters.

## Output behavior
- On success, the capability returns formatted local time text.
- It also updates the device clock from the network.
- On failure, the capability returns an error string similar to:
  - `Error: failed to fetch time (...)`

## Timezone notes
- The returned time is formatted using the device's currently configured timezone.

## Recommended workflow
1. Decide whether the user needs real current time or just a stable explanation about time handling.
2. Call `get_current_time` with `{}`.
3. Return the formatted result to the user.

## Common failure causes
- Expecting `get_current_time` to accept parameters; it does not.
- Calling it when network access is unavailable, causing sync failure.

## Examples

Get current local device time:

```json
{}
```

Use before a time-sensitive answer:

```json
{}
```
