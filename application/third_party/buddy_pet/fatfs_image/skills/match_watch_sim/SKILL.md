---
{
  "name": "match_watch_sim",
  "description": "Generate simulated football match data and push it to Match Watch UI through the standard provider path.",
  "metadata": {
    "cap_groups": [
      "cap_match_watch"
    ],
    "manage_mode": "readonly"
  }
}
---

# Match Watch Sim Provider

Use this skill when the user asks to test, preview, or simulate Match Watch without relying on a real football API.

This is a provider skill. It generates normalized matches and calls `match_watch_push_data`; Match Watch UI only displays the result.

## Command Boundary

For a normal simulation request, make exactly one native call:
`match_watch_provider_start` with `path`, `args`, and `name` in the same JSON
object. After it succeeds, stop.

Do not call `match_watch_open`, `match_watch_set_favorite`,
`match_watch_status`, `web_search`, `lua_run_script`, `lua_run_script_async`, or
`lua_stop_all_async_jobs` before or after provider start.

When the UI should open, pass `open:true` in provider `args`. The simulation
provider script opens the UI itself after it successfully pushes data, so the
outer `match_watch` entry skill must not add an extra `match_watch_open` call.

Start the provider session with `match_watch_provider_start`:

```json
{"path":"{CUR_SKILL_DIR}/scripts/watch.lua","args":{"open":true},"name":"match_watch_sim"}
```

Rules:

- Use `match_watch_provider_start` so simulation replaces the current provider job.
- There is no native simulation capability; use this provider skill directly.
- The provider pushes `schema_version:1` normalized data through `matches`.
- Do not answer that simulation started unless `match_watch_provider_start` succeeds.
- Stop simulation by starting another Match Watch provider, or by calling `match_watch_provider_stop` if the user explicitly asks to stop polling.
- The default favorite team is `Argentina`; pass `team` to focus another simulated team.
