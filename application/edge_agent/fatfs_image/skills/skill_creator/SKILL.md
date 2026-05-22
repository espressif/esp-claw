---
{
  "name": "skill_creator",
  "description": "Author a new runtime on-device skill on explicit user request, using the current directory-based SKILL.md framework.",
  "metadata": {
    "cap_groups": [
      "cap_files",
      "cap_skill"
    ],
    "manage_mode": "readonly"
  }
}
---

# Skill Creator

Create or repair runtime skills on-device. Use this skill only when the user explicitly asks to save, create, teach, or fix a reusable skill.

## Current Skill Framework

Every skill is a directory under `/fatfs/skills/`:

- Required document path: `/fatfs/skills/<skill_id>/SKILL.md`
- `register_skill.file` must be exactly `<skill_id>/SKILL.md`
- `SKILL.md` must start at byte 0 with JSON frontmatter wrapped by `---`
- Runtime skills must set `metadata.manage_mode` to `runtime`
- `metadata.cap_groups` controls which capability groups become visible when the skill is active
- Do not create or edit `skills_list.json`; the catalog is rebuilt from `SKILL.md` metadata

If a skill file lacks this frontmatter, Active Skill Docs fails with `ESP_ERR_INVALID_ARG` and logs `invalid metadata header`.

## When to Use

Use this skill only when the user says things like:

- "Save this as a skill"
- "Create a skill for ..."
- "Teach yourself to ..."
- "Fix this broken skill"
- "以后我说 ... 就按这个流程做"

Do not create skills during normal chat.

## Hard Rules

1. A skill is a prompt-time playbook for the agent, not a router automation.
2. For user-text-triggered actions, list direct tool calls the agent must emit. Do not add router rules for IM text triggers.
3. `router.match.text` is strict equality, not regex.
4. Built-in IM agent rules consume normal IM text before user-added rules see it.
5. Never claim a tool fired unless its tool call returned ok in the current turn.
6. Never write `skills/<skill_id>.md`; that is the old format and will break the current framework.
7. Never register with `file="<skill_id>.md"`; always use `file="<skill_id>/SKILL.md"`.

## Creation Flow

Follow this exact order.

### 1. Confirm Metadata

Ask for any missing fields:

| Field | Requirement | Example |
|---|---|---|
| `skill_id` | lowercase snake_case, no slash | `night_mode` |
| `summary` | one English sentence, <= 240 chars | `Dim the screen and clear the LCD when the user says good night.` |
| `cap_groups` | tools needed when active | `["cap_lua"]` |
| `triggers` | user phrases that should activate it | `good night`, `晚安` |
| `steps` | exact tool calls and arguments | `lua_run_script ...` |

If the procedure is vague or not repeatable, ask for clarification before creating the skill.

### 2. Check for Collision

Call:

```json
{}
```

with `list_skill` and verify `skill_id` is not already present.

If it exists, ask the user whether to choose another id or unregister the old runtime skill. Do not overwrite silently.

### 3. Register the Runtime Skill Skeleton

Call `register_skill` before writing full content. This creates the directory and a valid minimal `SKILL.md`, reloads the registry, and records `cap_groups` for the active-skill tool visibility path.

Example:

```json
{
  "skill_id": "night_mode",
  "file": "night_mode/SKILL.md",
  "summary": "Dim the screen and clear the LCD when the user says good night.",
  "cap_groups": ["cap_lua"]
}
```

If `register_skill` fails, stop and report the error.

### 4. Write the Full SKILL.md

Overwrite the skeleton with `write_file` at this cap_files-relative path:

```text
skills/<skill_id>/SKILL.md
```

The content must use this structure:

```markdown
---
{
  "name": "<skill_id>",
  "description": "<same summary>",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "runtime"
  }
}
---

# <Skill Title>

<one-sentence purpose>

## When to Use
<trigger phrases and non-trigger cases>

## Execution Discipline
- This skill mandates real tool calls. You MUST emit them.
- Do not reply with confirmation before the tool calls return ok.
- If any call fails, report the actual error and stop unless the skill says otherwise.

## Tool Calls
1. `<tool_name>` with arguments `{...}` — purpose: <effect>

## Reply Template
After all tool calls return ok, reply with one short sentence.

## Example
User: "<trigger phrase>"
Agent tool calls:
1. tool_call: `<tool_name>` args=`{...}` -> ok
Agent reply: "<final sentence>"
```

The JSON frontmatter must be valid JSON. No comments, trailing commas, markdown, or prose are allowed inside the frontmatter block.

### 5. Verify the File

Call `read_file` on:

```text
skills/<skill_id>/SKILL.md
```

Check all of the following before continuing:

- First line is exactly `---`
- Metadata JSON parses visually as valid JSON
- `name` equals `skill_id`
- `metadata.manage_mode` is `runtime`
- `metadata.cap_groups` matches the `register_skill` call
- The second closing `---` exists before the markdown body

If verification fails, rewrite the file immediately.

### 6. Activate and Demo

Call:

```json
{"skill_ids":["<skill_id>"]}
```

with `activate_skill`, then run the described workflow once if safe for the device. Base the final reply on real tool return values.

### 7. Send the Receipt

Tell the user:

- skill id
- path: `/fatfs/skills/<skill_id>/SKILL.md`
- cap groups
- example trigger phrases

## Fixing a Broken Old Skill

If logs show `read doc <id>: invalid metadata header`, the active skill file is old-format or malformed.

1. Ask the user whether to repair it.
2. If it is a runtime skill, call `unregister_skill` for the broken id when allowed.
3. Recreate it using the Creation Flow above.
4. Do not create `skills/<id>.md` as a compatibility workaround.

## Example: Night Mode

Register skeleton:

```json
{
  "skill_id": "night_mode",
  "file": "night_mode/SKILL.md",
  "summary": "When the user says good night, dim the backlight and clear the LCD.",
  "cap_groups": ["cap_lua"]
}
```

Then write `skills/night_mode/SKILL.md`:

```markdown
---
{
  "name": "night_mode",
  "description": "When the user says good night, dim the backlight and clear the LCD.",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "runtime"
  }
}
---

# Night Mode

Dim the board for bedtime when the user says good night or 晚安.

## When to Use
Use for `good night`, `晚安`, or going-to-bed requests. Do not use for morning greetings.

## Execution Discipline
- This skill mandates real tool calls. You MUST emit them.
- Do not reply before both tool calls return ok.
- If either call fails, report the error.

## Tool Calls
1. `lua_run_script` with `{"script":"nm_cyd_c5_backlight","args":{"percent":5}}` — dim the LCD backlight.
2. `lua_run_script` with `{"script":"nm_cyd_c5_screen","args":{"mode":"clear"}}` — clear the LCD.

## Reply Template
After both calls return ok, reply: `Good night, dimmed for you.`
```