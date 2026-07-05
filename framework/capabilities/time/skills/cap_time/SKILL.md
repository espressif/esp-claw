---
{
  "name": "cap_time",
  "description": "Return current local device time with SNTP sync only when the clock is invalid.",
  "metadata": {
    "cap_groups": [
      "cap_time"
    ],
    "manage_mode": "readonly"
  }
}
---

# Time

Use this skill when the user asks for the current date, time, weekday, local time, or needs exact relative-date grounding.

## Capability
- `get_current_time`: returns formatted current local device time.
- Input is always an empty object: `{}`.

## Rules
- Call `get_current_time` when exact current time matters.
- Do not pass parameters.
- Use the returned text as the source of truth for the answer.

## Output
- Success: formatted local time, including date, time, timezone, and weekday.
- Failure: an error string such as `Error: failed to get time (...)`, usually because time sync failed or the clock is invalid.
