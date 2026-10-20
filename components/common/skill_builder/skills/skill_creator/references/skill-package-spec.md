# Claw Skill Package Spec

This document defines the package contract shared by firmware-provided and runtime-installed Skills.

## Package Layout

```text
skills/
└── skill_id/
    ├── SKILL.md
    ├── references/
    │   └── guide.md
    ├── scripts/
    │   └── action.lua
    └── assets/
        └── icon.jpg
```

- `skills/<skill_id>/` is one complete Skill package.
- `SKILL.md` is the only required file.
- `references/`, `scripts/`, `assets/`, and other package-local files are optional.
- A Skill must not reference files owned by another Skill or an App.

## `SKILL.md`

`SKILL.md` must start with JSON frontmatter:

```md
---
{
  "name": "skill_id",
  "description": "Short capability description.",
  "author": "bob",
  "metadata": {
    "category": ["game", "ui"],
    "tags": ["arcade", "demo"],
    "peripherals": ["display"],
    "cap_groups": ["cap_lua"]
  }
}
---

# Skill Title
```

Rules:

- The frontmatter must be wrapped by `---` and contain one valid JSON object.
- The document body must contain exactly one H1 heading.
- `name` must be a non-empty string that exactly matches the package directory name.
- `description` must concisely describe the user intent and any prerequisite that affects whether the Skill applies. Prefer common user wording over internal script or module names.
- `author` is optional. When present, it must be a non-empty string. An author containing angle brackets must use `Name <email>` with exactly one valid email address.
- `metadata` is optional and must be an object when present.
- `metadata.cap_groups` must be an array of unique, non-empty strings when present. Declare only capability groups required by the workflow.
- `metadata.category` must contain at least one allowed category when present.
- `metadata.peripherals` may contain allowed peripheral values.
- `metadata.tags` must be an array of strings when present. Tags are free-form but must not repeat category or peripheral values.
- Include catalog metadata such as category, peripherals, and tags only when the target catalog or user supplies valid values; do not invent allowlist entries.
- Additional root or metadata fields are allowed for forward compatibility. The device runtime ignores fields it does not consume; preserve fields that are not being intentionally changed.

## Package Paths

When a Skill is loaded, the runtime replaces `{CUR_SKILL_DIR}` in its Markdown body with the absolute directory of that Skill package. It does not expand the placeholder inside frontmatter.

Use the placeholder for every package-owned file:

```text
{CUR_SKILL_DIR}/references/guide.md
{CUR_SKILL_DIR}/scripts/action.lua
{CUR_SKILL_DIR}/assets/icon.jpg
```

- File tools require absolute paths, so do not pass package-relative paths.
- Do not hard-code `/system/skills`, `/fatfs/skills`, a component source path, or another device-specific location.
- Do not use `..` to escape the package directory.
- Writable files created by Lua scripts must use `storage.get_root_dir()` and `storage.join_path(...)`.

## Lua Scripts

Place Skill-owned Lua scripts under `scripts/` and invoke them through `{CUR_SKILL_DIR}`.

- Scripts are read-only package resources after a firmware-provided Skill is installed in SYSTEM.
- A script may only use Lua modules and native capabilities already present in the firmware.
- If execution fails, report the actual error instead of repeatedly changing arguments and retrying.
- User-facing actions belong in a Skill workflow rather than a module test directory.

## Identity And Lifecycle

- Skill ids must contain 1–63 ASCII letters, digits, underscores, or hyphens. Prefer lowercase ids.
- Skill ids and package output paths must be unique within a firmware build.
- Do not expose the same user-facing action through multiple Skills or script indexes.
- Firmware-provided Skills are installed under the read-only SYSTEM Skill root.
- Runtime-installed Skills are stored under the writable DATA Skill root.
- A DATA Skill shadows a SYSTEM Skill with the same id; removing the DATA override reveals the SYSTEM Skill again.
- Malformed or unreadable Skills are logged and skipped without preventing other valid Skills from loading.
