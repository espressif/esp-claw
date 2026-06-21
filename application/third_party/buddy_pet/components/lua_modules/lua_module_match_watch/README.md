# Lua Match Watch

This module controls the Match Watch runtime from Lua through a small command
surface. UI rendering, pet animation, networking, and state-machine behavior
stay inside the native runtime. Provider `watch.lua` scripts own score polling
and push normalized data with `match_watch.push_data_json(json)`; this module
does not provide a native score refresh entrypoint. Normalized payloads should
use `schema_version: 1`; see
`components/common/match_watch_runtime/PROVIDER_CONTRACT_CN.md`.

## How to call

- Import it with `local match_watch = require("match_watch")`
- Call `match_watch.open()` to show the Match Watch screen
- Call `match_watch.close()` to close it
- Call `match_watch.set_favorite(team)` to select a favorite team
- Call `match_watch.set_favorite(nil)` or `match_watch.set_favorite("")` to clear the favorite team
- Call `match_watch.set_reminders(true)` to enable kickoff, goal, and full-time reminders
- Call `match_watch.set_reminders(false)` to disable match reminders
- Call `match_watch.push_data_json(json)` to push normalized external match data from a provider skill

All functions return `true` on success and raise a Lua error on failure.

## Example

```lua
local match_watch = require("match_watch")

match_watch.open()
match_watch.set_favorite("Argentina")
match_watch.set_reminders(true)
```

```lua
match_watch.push_data_json([[
{
  "schema_version": 1,
  "provider": "worldcup",
  "competition": "FIFA World Cup",
  "team": "Argentina",
  "matches": [
    {"match_no": 1, "stage": "group", "home": "Argentina", "away": "Brazil", "score": "0-0", "state": "upcoming"}
  ]
}
]])
```
