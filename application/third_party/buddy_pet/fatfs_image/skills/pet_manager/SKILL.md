---
{
  "name": "pet_manager",
  "description": "Download, list, select, switch, and control buddy actions for the on-screen companion. Chinese users may call it 伙伴 or 兄弟.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Buddy Manager

Use this skill when the user asks to download a buddy, download a
`codex-pets.net` buddy package, install/select a buddy by id, switch the
selected buddy, list installed buddies, return to the built-in buddy, or control
the current buddy action from IM. Chinese user wording such as "伙伴" or "兄弟"
means this same on-screen buddy.

For `codex-pets.net` links or short buddy ids such as `messi`, download
immediately. Do not ask for replacement confirmation. Do not list buddies before
downloading.

If the user says "下载 buddy", "下载伙伴", or "下载兄弟" without a link or id,
list installed buddies once and ask for a buddy id or a
`https://codex-pets.net/#/pets/[id]` link. Do not activate `skills_lab_search`;
ESP-Claw Skill Hub skills are not buddy packages.

Downloaded buddy packages are discovered from mmap-assets `.bin` files under
`/fatfs/skills/pet_module/`. Metadata can be embedded as `pet.json` in the bin
or supplied as a sidecar `[id].pet.json`. The package should contain
`index.json` and the action assets referenced by that index.

The buddy is managed by the Pet Buddy runtime. Scene modules such as Match Watch
attach the same selected buddy through that runtime. Buddy Manager must not open,
close, or orchestrate scene modules.

Example package:

```text
/fatfs/skills/pet_module/kapi-striker.bin
```

Embedded `pet.json` example:

```json
{
  "id": "kapi-striker",
  "name": "Kapi Striker",
  "title": "worldcup",
  "profile": {
    "team": "Argentina"
  }
}
```

`title` and `profile` come from the metadata. Scene modules may read
`profile.team` from the selected buddy. Existing fixed buddy bins that expose
`country` are still accepted and normalized to `profile`.

When selecting a buddy, use `select_pet.lua` only. It selects the buddy and lets
currently active scene modules reload or interpret buddy metadata through Pet
Buddy hooks. Do not manually call Match Watch, provider, or router tools from
this skill.

When downloading a buddy, use `download_pet.lua` only. The script streams the
converted bin to `/fatfs/skills/pet_module/[id].bin.download`, installs it after
success, refreshes the registry, and selects it. Do not call `list_dir`,
`read_file`, `write_file`, `delete_file`, `http_request`, debug scripts, or
manual Match Watch provider tools before or after the script.

Do not use `lua_run_script_async`, `lua_get_async_job`, or
`lua_tail_async_job` for buddy downloads. Use one foreground `lua_run_script` call
to `download_pet.lua`; the script already owns the HTTP conversion, file write,
registry refresh, and selection flow.

## List Installed Buddies

Call `lua_run_script`:

```json
{"path":"{CUR_SKILL_DIR}/scripts/list_pets.lua","args":{}}
```

## Download Buddy

Call `lua_run_script` immediately when the user sends a valid link:

```json
{"path":"{CUR_SKILL_DIR}/scripts/download_pet.lua","args":{"url":"https://codex-pets.net/#/pets/kapi-striker"}}
```

Call the same script with `id` when the user sends a buddy id such as "下载 messi":

```json
{"path":"{CUR_SKILL_DIR}/scripts/download_pet.lua","args":{"id":"messi"}}
```

## Select A Buddy

Call `lua_run_script` with the buddy id:

```json
{"path":"{CUR_SKILL_DIR}/scripts/select_pet.lua","args":{"id":"kapi-striker"}}
```

## Clear Selection

Call `lua_run_script`:

```json
{"path":"{CUR_SKILL_DIR}/scripts/clear_pet.lua","args":{}}
```

## Control Buddy Action

Call `lua_run_script` with an action, or call the native `pet_buddy_action`
capability directly when available. Both routes use the unified Pet Buddy action
API. The action is applied to the buddy attached by the active module: Match
Watch or the empty buddy module. If no module has attached the buddy, the empty
buddy module is opened first.

Supported user-facing actions:

- `idle`
- `jumping`
- `Run right` / `running-right`
- `Run left` / `running-left`
- `Runing` / `running`
- `Waving` / `waving`
- `failed`
- `lose`
- `waiting`
- `Review` / `review`

```json
{"path":"{CUR_SKILL_DIR}/scripts/action_pet.lua","args":{"action":"jumping"}}
```
