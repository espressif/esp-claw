---
{
  "name": "skill_creator",
  "description": "Create, update, or remove an on-device runtime Skill when the user explicitly asks to manage a Skill; not for generic feature requests, Apps, or native firmware capabilities.",
  "metadata": {
    "cap_groups": [
      "cap_files",
      "cap_lua",
      "cap_skill_manage"
    ]
  }
}
---

# Skill Creator

Use this skill only when the user explicitly asks to create, update, or remove an on-device runtime Skill.

Do not activate it for a generic request such as "add a feature" or "support X". First use an existing domain Skill when one covers the request. Use `app_creator` for launcher-visible Apps. If the request needs a new C capability, driver, Kconfig option, firmware component, or build-time SYSTEM Skill, explain that firmware development is required and use `plan_mode` when available.

This workflow manages runtime Skills under the writable DATA root. It does not edit firmware source files or create native capabilities.

## Resolve The Runtime Skills Root

Before reading or writing a runtime Skill, run:

```json
{"path":"{CUR_SKILL_DIR}/scripts/resolve_skills_root.lua","args":{}}
```

Treat the returned absolute path as `<SKILLS_ROOT>`. Do not assume `/fatfs`; DATA may be mounted on an SD card. If the path cannot be resolved, report the error and stop.

## Choose The Skill Shape

- Use an instruction-only Skill when existing capabilities already provide the required operations.
- Add references when substantial guidance is needed only for part of the workflow.
- Add one or more Lua scripts when reusable or deterministic logic is needed and the required Lua modules already exist.
- Do not claim that a Skill or Lua script adds firmware support that is not already compiled into the device.

For Lua-backed Skills, also activate `lua_package_authoring` and follow its Skill-script rules. Keep this skill responsible for the package layout and final publication.

## Skill Contract

Before creating or updating a Skill, read `{CUR_SKILL_DIR}/references/skill-package-spec.md` and follow the complete package contract. Do not load the reference for removal-only requests.

Store each runtime Skill under `<SKILLS_ROOT>/<skill_id>/`. Do not create or overwrite a Skill whose id already exists unless the user explicitly requested that exact update.

Minimal document:

```md
---
{
  "name": "skill_id",
  "description": "Describe the user request this Skill handles.",
  "metadata": {
    "cap_groups": []
  }
}
---
```

After the frontmatter, add exactly one H1 title and only the instructions needed to perform the documented workflow.

## Create Or Update

1. Resolve `<SKILLS_ROOT>` and inspect the target Skill before changing it.
2. Read the package spec and confirm that the requested behavior can be implemented with currently available capabilities or Lua modules.
3. Design the smallest complete set of files. Do not add placeholder references, scripts, or assets.
4. For a new Skill, write supporting files first and `SKILL.md` last. For an update, inspect the existing payload, preserve unrelated content, write changed supporting files first, and update `SKILL.md` last.
5. Read back the changed text files and check paths, frontmatter JSON, Skill id, capability groups, and documented script arguments.
6. Call `publish_skill` with only the final id:

```json
{"skill_id":"skill_id"}
```

7. Treat the tool result as authoritative. Report publication errors directly and do not claim success when publication fails.

Publishing refreshes the runtime registry; it does not build firmware or add missing capabilities.

## Remove

Remove a Skill only when the user explicitly requests deletion of that exact runtime Skill. Inspect the target first, then call:

```json
{"skill_id":"skill_id"}
```

with `remove_skill`. If the tool reports `readonly_skill`, explain that firmware-baked SYSTEM Skills cannot be removed at runtime.
