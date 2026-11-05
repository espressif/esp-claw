---
{
  "name": "skills_lab_downloader",
  "description": "Download a Skill or App from the ESP-Claw hub by exact kind and id, check required peripherals, and publish it on the device.",
  "metadata": {
    "cap_groups": [
      "cap_lua",
      "cap_http_request",
      "cap_boards",
      "cap_skill_manage",
      "cap_app_manage"
    ]
  }
}
---

# Skills Lab Downloader

Download a Skill or App by its exact `kind` and `id` from `skills-lab.esp-claw.com`.

## Rules

- Require `kind` (`skill` or `app`) and the exact package `id`. Never infer kind from a title or default to Skill. If unknown, use `skills_lab_search` or ask the user.
- Use `{CUR_SKILL_DIR}/scripts/download_package.lua` for both metadata and file downloads. Enable `cap_http_request`; if the host is blocked, tell the user to add `*.esp-claw.com` to the Web Console HTTP allowlist.
- Read the returned metadata yourself; do not ask Lua to parse JSON. Verify `kind` and `name` match the requested kind and id. Stop on missing or mismatched metadata, HTTP errors, or missing `files`.
- Before installation, compare `metadata.peripherals` with `board_hardware_info`. If required peripherals are missing, stop unless the user explicitly approves force installation.
- Pass the complete metadata `files` array unchanged. It includes the manifest and nested resource paths; do not use `extra_files` or reconstruct a list from prose.
- After all downloads succeed, call `publish_app` for an App or `publish_skill` for a Skill. Never publish after a download failure. Only report installation complete after publication succeeds; otherwise report that files were downloaded but publication failed.

## Workflow

1. Fetch metadata using `lua_run_script`:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_package.lua",
  "args": { "action": "fetch_metadata", "kind": "app", "id": "example-app" }
}
```

2. Check package identity and peripherals, then install using the exact `files` returned by the server. Set `timeout_ms` to 300000 for the complete download; the default 60-second script timeout may be too short for resource-heavy Apps. For example:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_package.lua",
  "timeout_ms": 300000,
  "args": {
    "action": "install",
    "kind": "app",
    "id": "example-app",
    "files": ["launcher.json", "scripts/main.lua", "assets/audio/tone.pcm"]
  }
}
```

3. On success, call `publish_app` with `{"app_id":"example-app"}`. For a Skill, use `kind: "skill"` in both script calls and finish with `publish_skill({"skill_id":"example-skill"})`.

## Storage and failures

- Skills use `/raw/<id>/` on the server and `<DATA>/skills/<id>/` locally; `SKILL.md` is required.
- Apps use `/raw/apps/<id>/` on the server and `<DATA>/apps/<id>/` locally; `launcher.json` is required.
- Files stream directly to storage with a 20-second request timeout and a 10 MiB per-file limit. Nested directories are created automatically; paths must remain inside the package.
- A failed or truncated download is removed. Previously downloaded files may remain; installation is not atomic and retrying overwrites files. Publication does not launch the App or activate the Skill.
