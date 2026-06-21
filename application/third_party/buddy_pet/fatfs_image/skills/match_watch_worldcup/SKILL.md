---
{
  "name": "match_watch_worldcup",
  "description": "Follow/watch FIFA World Cup national teams such as Argentina/阿根廷, fetch team schedule data, normalize it, and push it to Match Watch UI.",
  "metadata": {
    "cap_groups": [
      "cap_web_search",
      "cap_match_watch",
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Match Watch World Cup Provider

Use this skill when the user wants to watch, follow, refresh, or switch a FIFA World Cup national-team match in Match Watch. Examples: "关注阿根廷", "看阿根廷世界杯", "关注法国队比赛", "follow Argentina", "watch Brazil World Cup".

This skill is a provider skill. It fetches and normalizes data; the Match Watch
UI only displays the result.

## Command Boundary

For a normal follow/watch request, make exactly one native call:
`match_watch_provider_start` with `path`, `args`, and `name` in the same JSON
object. After it succeeds, stop.

Do not call `match_watch_open`, `match_watch_set_favorite`,
`match_watch_status`, `web_search`, `lua_run_script`, `lua_run_script_async`, or
`lua_stop_all_async_jobs` before or after provider start.

When the UI should open, pass `open:true` in provider `args`. The provider script
opens the UI itself after it successfully pushes data, so the outer `match_watch`
entry skill must not add an extra `match_watch_open` call.

Rules:

- Supported teams are the 48 FIFA World Cup 2026 participants defined in `scripts/teams.lua`. Use `team: "<English name>"` or `team_slug: "<slug>"`.
- For console/manual calls, prefer ASCII `team_slug` to avoid serial-console encoding issues.
- For "关注阿根廷" or "看阿根廷", use `team_slug: "argentina"` and `open: true`.
- Chinese names are accepted for `team` when listed in `scripts/teams.lua`.
- Start `/fatfs/skills/match_watch_worldcup/scripts/watch.lua` with `match_watch_provider_start`; this is the normal follow/refresh path.
- Always include `args` in the same `match_watch_provider_start` call. Never call `match_watch_provider_start` with only `path` for a team-follow request.
- OpenLigaDB fetch/normalize/push logic lives in `scripts/openliga_worldcup.lua`; `watch.lua` is a thin wrapper that runs the polling loop.
- Provider-specific args such as `team_slug`, `team`, and `team_code` are validated inside `openliga_worldcup.lua`.
- If the user also asks to open the UI, pass `open=true`; do not call `match_watch_open` separately.
- For one-shot provider debugging/manual checks, use `watch.lua` with `max_cycles: 1` instead of starting a long-lived poll loop.
- `watch.lua` keeps running after transient network/API failures with backoff retries. It also keeps running after the current match finishes, displays the last result, then checks at `idle_poll_ms` for newly released next-round fixtures.
- Do not call `lua_run_script_async` or `lua_stop_all_async_jobs` directly for provider switching. `match_watch_provider_start` is the switch mechanism.
- Do not call `web_search` for schedules or group info. Network fetching happens inside the provider through `http_request`; `cap_web_search` exists only so the Lua provider can fetch its source data.
- Do not use this skill for club-team requests. This provider only handles World Cup national teams.
- Do not answer that a team was followed/watched unless `match_watch_provider_start` succeeds.

Common aliases (full list in `scripts/teams.lua`): `bra`/`brasil`/`巴西` → Brazil; `arg`/`阿根廷` → Argentina; `fra`/`法国` → France; `eng`/`英格兰` → England; `esp`/`西班牙` → Spain; `por`/`葡萄牙` → Portugal; `ger`/`德国` → Germany.

Provider session examples:

```json
{"path":"/fatfs/skills/match_watch_worldcup/scripts/watch.lua","args":{"team_slug":"argentina","open":true,"poll_ms":60000,"idle_poll_ms":600000},"name":"match_watch_worldcup"}
```

```json
{"path":"/fatfs/skills/match_watch_worldcup/scripts/watch.lua","args":{"team":"France","open":true,"poll_ms":60000,"idle_poll_ms":600000},"name":"match_watch_worldcup"}
```

Debug one-shot only:

```json
{"path":"{CUR_SKILL_DIR}/scripts/watch.lua","args":{"team_slug":"brazil","open":true,"max_cycles":1}}
```

The normalized data pushed to Match Watch uses:

- `schema_version`: `1`
- `competition`: `FIFA World Cup`
- `stage`: `group` for team-feed group fixtures
- `state`: `upcoming`, `live`, `half_time`, or `full_time`
- `watch.lua` keeps refreshing: it waits until close to kickoff, polls every `poll_ms` during live play, and polls every `idle_poll_ms` after full time while waiting for new fixtures.
