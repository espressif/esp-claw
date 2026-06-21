---
{
  "name": "match_watch",
  "description": "Control Match Watch UI. Real competition data must come from provider skills.",
  "metadata": {
    "cap_groups": [
      "cap_match_watch"
    ],
    "manage_mode": "readonly"
  }
}
---

# Match Watch Entry

Use this skill as the Match Watch entry/router. It can directly control generic
UI state, but real competition follow/watch requests must be delegated to a
provider skill.

This product stays in Match Watch as the foreground scene. If the user asks to
return to the home page, treat Match Watch as the home screen and keep it open.
Do not call removed Buddy Home scripts, `list_dir`, `read_file`, `write_file`,
or temporary Lua scripts for that request.

## Provider Routing Commands

- National teams or "世界杯" follow/watch requests must switch to
  `match_watch_worldcup`.
- The provider skill must make exactly one `match_watch_provider_start` call.
- Do not answer that a team is followed/watched until `match_watch_provider_start`
  succeeds.
- If provider start succeeds, stop immediately. Do not add `match_watch_open`,
  `match_watch_set_favorite`, `match_watch_status`, `web_search`, or Lua job calls.
- Provider scripts own data fetch, favorite team selection, data push, and opening
  the UI when `open=true`.
- Provider scripts must push normalized payloads with `schema_version:1` and
  `matches`; provider-specific args are validated by the provider script, not by
  native `cap_match_watch`.
- Club-team provider support has been removed. If the user asks for club-team
  match tracking, explain that no provider is installed.

Routing examples:

- `巴西队`/`Brazil` -> `match_watch_worldcup`, `team_slug:"brazil"`.
- `阿根廷`/`Argentina` -> `match_watch_worldcup`, `team_slug:"argentina"`.
- `法国队`/`France` -> `match_watch_worldcup`, `team_slug:"france"`.

## Direct UI Commands

- Open UI only when no provider-backed team refresh/follow is requested:
  `match_watch_open`.
- To open/sync Match Watch from the current selected buddy profile, call
  `/fatfs/skills/match_watch/scripts/sync_from_pet.lua`.
- Close UI only when the user explicitly asks to close Match Watch.
- Enable reminders: `match_watch_set_reminders`.
- Status: `match_watch_status`, only when the user explicitly asks for status.
- Stop provider polling: `match_watch_provider_stop`, only when the user explicitly
  asks to stop polling.

Selected buddy profile sync example:

```json
{"path":"/fatfs/skills/match_watch/scripts/sync_from_pet.lua","args":{"open":true},"timeout_ms":3000}
```

Sync priority is: current Match Watch `favorite_team`, selected buddy
`profile`/`country`, any local buddy with `profile`/`country`, then
`fallback_team`. For example:
`{"open":true,"selected_only":true,"fallback_team":"Argentina"}`.
