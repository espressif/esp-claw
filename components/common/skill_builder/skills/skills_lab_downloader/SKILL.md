---
{
  "name": "skills_lab_downloader",
  "description": "Download a skill from the ESP-Skill hub by exact skill name, verify required peripherals against board_hardware_info, and save it into the on-device skills directory.",
  "metadata": {
    "cap_groups": [
      "cap_lua",
      "cap_web_search",
      "cap_boards"
    ],
    "manage_mode": "readonly"
  }
}
---

# Skills Lab Downloader

Use this skill when the user wants to install a skill from the ESP-Claw's Skill hub at `skills-lab.esp-claw.com`.

## When to use
- The user wants to download or install a skill from the ESP-Claw's Skill hub.
- The user already knows the exact skill name, or needs a reminder that installation requires the exact skill name instead of the visible title.

## Hard rules
1. Installation requires the exact skill name, not the skill title.
2. A valid skill name contains only `A-Z`, `a-z`, `0-9`, `_`, or `-`.
3. Before downloading, make sure `cap_web_search` is enabled and available. If it is unavailable, stop and tell the user to enable web search first.
4. Use `{CUR_SKILL_DIR}/scripts/download_skill.lua` for hub download requests. Do not manually recreate the HTTP flow outside this script.
5. First fetch `_metadata.json` and return it to yourself as a raw JSON string. Treat that string as the source of truth for install decisions.
6. If metadata fetch returns a 404 result, tell the user the skill does not exist and stop.
7. If metadata fetch fails with an allowlist error such as `HTTP allowlist is empty` or `host 'skills-lab.esp-claw.com' is not in allowlist`, tell the user to add `*.esp-claw.com` to the Web Console allowlist and stop.
8. Before installation, inspect `metadata.peripherals`. If it is non-empty, activate `board_hardware_info` and compare every listed peripheral against the current board device inventory.
9. If one or more required peripherals are unavailable, do not install by default. Tell the user which peripherals are missing and stop unless the user explicitly asks to force install.
10. Only after compatibility is confirmed, or the user explicitly approves force install, run the install step.
11. After a successful install, tell the user the skill has been installed.
12. Do not ask Lua to parse `_metadata.json`. The model must read the JSON string, make the install decision, and pass the file lists to Lua as structured args.

## Workflow
1. Ask for the exact skill name if the user did not provide it.
2. Validate that the skill name matches `^[A-Za-z0-9_-]+$`.
3. Fetch metadata with `lua_run_script`:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_skill.lua",
  "args": {
    "action": "fetch_metadata",
    "skill_name": "example-skill"
  }
}
```

4. Read the returned JSON string from `_metadata.json`.
5. Check `metadata.peripherals`:
   - If empty, continue.
   - If non-empty, read `board_hardware_info` and verify every peripheral is available on this device.
6. If peripherals are missing, explain the mismatch and ask whether the user wants to force install.
7. Install by passing the validated file lists from `_metadata.json`:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_skill.lua",
  "args": {
    "action": "install",
    "skill_name": "example-skill",
    "skill_name_from_metadata": "example-skill",
    "extra_files": {
      "references": ["ref1.md"],
      "scripts": [],
      "assets": []
    }
  }
}
```

8. Report completion to the user.

## Install paths
- Hub `https://skills-lab.esp-claw.com/file/<skill_name>/SKILL.md` is saved to `/fatfs/skills/<skill_name>/SKILL.md`.
- Hub `https://skills-lab.esp-claw.com/file/<skill_name>/<group>/<file>` is saved to `/fatfs/skills/<skill_name>/<group>/<file>`.

## Notes
- `extra_files.references`, `extra_files.scripts`, and `extra_files.assets` are additional files that must be downloaded when present.
- The installer script expects the model to pass the validated `extra_files` object directly from `_metadata.json`.
- Do not guess or normalize a similar-looking title into a skill name. Ask the user for the exact skill name when needed.
