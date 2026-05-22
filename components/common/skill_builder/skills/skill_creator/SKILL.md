---
{
  "name": "skill_creator",
  "description": "Create or update model-invoked functions/features/skills, workflows, and Lua-backed capabilities.",
  "metadata": {
    "cap_groups": [
      "cap_skill"
    ],
    "manage_mode": "readonly"
  }
}
---

# Skill Creator

Use this skill when the user wants to add a new reusable feature that the model should be able to invoke later as a skill, including tool-like workflows, project-specific capabilities, Lua-backed automations, or feature requests phrased as "add a function", "support doing X", "make the model able to X", "create a tool for X", or "新增一个功能".

Also use this skill when the user asks to create, register, update, or remove a skill, or asks how to create a skill that includes Lua files.

## Core Rules

Every skill creation flow those mandatory phases:

1. Create the complete source skill files.
2. (Options for Lua-backed skills) Create the complete bundled Lua files under the skill-owned `scripts/` directory and document how the skill invokes Lua.
3. Immediately call `register_skill` so the newly created skill is added to the active skill registry.

Skills with source files live under a `skills/<skill_id>/` directory that is included in the firmware or file image build. They may require rebuilding or regenerating the image that contains skills.

Skill id rules:

- Use lowercase letters, digits, underscores, or hyphens.
- Do not use spaces, slashes, backslashes, absolute paths, or `..`.
- Make the id match the user-facing behavior rather than the implementation.
- Do not reuse an existing skill id.

Intent description rules:

- Describe user intent, not just internal implementation.
- Include trigger words users are likely to say.
- Include prerequisites when they are required.
- Keep it to one concise sentence.
- Do not summarize a skill as only running a script or calling a tool.

File generation rules:

- Create skill files by directly writing the needed files. Do not run a bundled preparation script or template generator.
- Generate `SKILL.md` from the final behavior, not from a generic unchanged template.
- Generate optional bundled files directly, such as `scripts/<name>.lua`, `references/<name>.md`, or assets.
- If the environment cannot write files or cannot call `register_skill`, provide the target relative paths and full file contents, report the blocker, and do not claim the skill was fully created and registered.

Registration rules:

- Call `register_skill` for every newly created skill as the final creation step.
- Register with `file` set exactly to `<skill_id>/SKILL.md`.
- Use the tool result as the source of truth for registered skill metadata.
- If registration fails, report the returned error directly and do not claim the skill was registered.

## Skill File Layout

Use this source layout for a skill with files:

```text
component_or_app/
└── skills/
    └── skill_id/
        ├── SKILL.md
        ├── references/
        │   └── guide.md
        ├── scripts/
        │   └── action.lua
        └── assets/
            └── image.bin
```

Valid source locations are project-specific. Common locations include component skill directories and application FATFS image skill directories. Do not create skills outside a directory that the current build or file-image generation path includes.

Rules:

- `skills/<skill_id>/SKILL.md` is required.
- `references/`, `scripts/`, `assets/`, and other subdirectories are optional.
- `skills/<skill_id>/scripts/*.lua` files are optional, but all bundled Lua files must live under `scripts/`.
- The `SKILL.md` frontmatter `name` must match `skill_id`.
- Reference bundled scripts in the skill body with `{CUR_SKILL_DIR}/scripts/<name>.lua`.
- Every skill that includes one or more Lua scripts must document how each script is invoked, including the exact `{CUR_SKILL_DIR}/scripts/...` path, args schema, sync or async execution mode, timeout policy, exclusive group if any, and output/error handling.
- Do not reference source-tree paths or FATFS output paths directly.

## Create A Skill

Flow:

1. Decide the user-facing behavior, title, description, capability groups, whether Lua is needed, and bundled file names.
2. If Lua is needed, keep `skill_creator` as the workflow owner, activate `cap_lua` only for Lua path and runtime rules, then read `{CUR_SKILL_DIR}/references/write_lua.md` for authoring patterns and `{CUR_SKILL_DIR}/references/run_lua.md` for run-tool semantics.
3. Check the target `skills/<skill_id>/` directory does not already exist unless the user explicitly asked to update or replace that skill.
4. Write the complete `SKILL.md` and any optional bundled files into a valid source `skills/<skill_id>/` directory.
5. Make semantic sections specific to the skill: trigger wording, prerequisites, `Recommended Flow`, args schema, and script behavior when Lua is used.
6. Register the skill using the flow below.
7. Tell the user the registered skill id.
8. Tell the user if a rebuild, file image regeneration, registry reload, or device restart may still be required before a newly created source skill is fully available.

Use this `SKILL.md` pattern for a Lua-backed skill:

````md
---
{
  "name": "skill_id",
  "description": "Describe the user-facing action and any prerequisites in one sentence.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Skill Title

Use this skill when the user asks to perform the specific user-facing action.

Run exactly one bundled Lua script with the Lua script execution capability.

If script execution returns an error, report that error directly to the user.
Do not retry with changed arguments or run another script in the same turn unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {}
}
```

## Tool Call Inputs

Default action:

```json
{"path":"{CUR_SKILL_DIR}/scripts/action.lua","args":{}}
```

## Recommended Flow

1. Confirm the script path is the bundled skill-local path `{CUR_SKILL_DIR}/scripts/action.lua`.
2. Validate or choose safe arguments from the documented args schema.
3. Run `{CUR_SKILL_DIR}/scripts/action.lua` with the resolved `args`.
4. Report the script result or error directly to the user.
````

For a non-Lua skill, omit Lua-specific sections and include only the capability groups and flow needed for the user-facing action.

## Create A Lua File

For reusable user-facing behavior, create or edit Lua only under the skill-owned `scripts/` directory and keep this skill as the workflow owner. Activate `cap_lua` together with or after this skill only to load Lua path rules, then read `{CUR_SKILL_DIR}/references/write_lua.md` for the Lua template, module documentation strategy, authoring rules, and quality rules. Read `{CUR_SKILL_DIR}/references/run_lua.md` when documenting how the skill invokes Lua, including sync or async execution, args, timeouts, names, exclusive groups, and output handling.

Do not create bare Lua files for ambiguous "add a feature" requests.

## Register Every Created Skill

Before registering a skill, choose:

- `skill_id`: stable, short, and based on the user-facing behavior.
- `file`: exactly `<skill_id>/SKILL.md`.
- `description`: the `SKILL.md` frontmatter description, one sentence describing when the skill should be used, including common user wording and critical prerequisites.

Flow:

1. Choose or edit the final `skill_id` and frontmatter `description`.
2. Ensure the complete source-file `SKILL.md` already exists at `skills/<skill_id>/SKILL.md`.
3. Call `register_skill` directly with `skill_id` and `file`.
4. If registration succeeds, tell the user the skill id that was registered.
5. If registration fails, report the returned error directly and do not claim the skill was registered.

The `register_skill` input has this shape:

```json
{"skill_id":"weather_alerts","file":"weather_alerts/SKILL.md"}
```

Use the tool result as the source of truth for the registered skill metadata.

## Update A Skill

When updating a skill, update its source files, then refresh registration:

1. Confirm the target `skill_id` and whether the user asked to update or replace that skill.
2. Update the existing skill files.
3. Call `unregister_skill` for the old `skill_id` when the registry requires replacement.
4. Call `register_skill` with the same `skill_id` and `file`.

If unregistering or registering fails, stop and report the error.

## Good Examples

Create and register a reminder skill:

```json
{"skill_id":"simple_reminders","file":"simple_reminders/SKILL.md"}
```

Create and register a board notes skill:

```json
{"skill_id":"board_notes","file":"board_notes/SKILL.md"}
```

Create and register a Lua skill when the user asks for scripts, assets, or detailed flow:

```text
components/example/skills/take_measurement/
├── SKILL.md
└── scripts/
    └── take_measurement.lua
```

## Bad Examples

Do not use implementation-only summaries:

```json
{"skill_id":"script_runner","file":"script_runner/SKILL.md"}
```

Do not use invalid paths:

```json
{"skill_id":"my skill","file":"../my skill/SKILL.md"}
```
