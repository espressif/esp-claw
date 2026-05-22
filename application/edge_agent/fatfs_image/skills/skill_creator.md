# Skill Creator

Let ESP-Claw **evolve itself** on-device: distill a recurring workflow into a new skill, write it to `/fatfs/skills/`, and register it in `skills_list.json`. Next time a similar request arrives, the agent picks up this long-term memory through `activate_skill` instead of re-reasoning from scratch.

## When to use

Only when the user **explicitly** asks for it, e.g.:

- "Save this as a skill / 把刚才的步骤保存成技能 / teach you a new skill"
- "Add / create / make a new skill for XXX"
- "From now on, when I ask you to do XXX, follow this procedure"
- The user has handed over a stable, reusable scaffold worth persisting

**Do not** spontaneously create skills during normal chat. Skills have a real cost: flash writes, prompt context, and long-term maintenance.

## Required capabilities

This skill depends on the following LLM-visible caps (already in the default whitelist):

- `cap_files` (`write_file`, `read_file`, `list_dir`) — read/write `/fatfs/skills/*.md`
- `cap_skill` (`register_skill`, `unregister_skill`, `list_skill`, `activate_skill`) — maintain `skills_list.json`
- Optional `cap_lua` (`run_lua_script` / `lua_save_script`): when the new skill needs to bundle a Lua script

## Hard rules — never violate

These are not preferences. They reflect actual router / capability source-code behaviour:

1. **A skill is a prompt-time playbook for the agent, NOT a router automation.**
   For any user-text-triggered action (e.g. "good night → dim backlight"), the skill body MUST list direct tool calls (`run_lua_script ...`) that the agent itself emits. **Never** use `add_router_rule` for IM-text triggers — see Hard rule 3.
2. **`router.match.text` is strict `strcmp` equality, not regex.**
   `"text": "/^(早安|good\\s*morning)$/i"` will NEVER match anything. The router has no regex engine. Only use `match.text` if you want exact full-string equality (rarely useful for chat).
3. **Built-in rule `im_any_message_agent` consumes every IM text message before any user-added rule sees it.**
   Any rule added via `add_router_rule` is appended to the end and will never fire on `event_type=message, content_type=text`. Do not even try to route IM text via router rules.
4. **Only invent an `event_type` after grepping the source.**
   Reading log lines like `I (...) touch_xpt2046: release at (40,19)` does NOT mean an event type `touch_xpt2046` exists. Many drivers only `ESP_LOG`. Before referencing an event type in `add_router_rule`, confirm it is published with `claw_event_publish*` somewhere. If you cannot confirm, do not write the rule.
5. **Never claim a tool fired without seeing its return value.**
   Do not reply "rule auto-triggered, backlight 100%" unless a `tool_call` actually returned ok in this same turn. Hallucinated success messages are forbidden.

## Workflow (follow strictly in order)

### 1. Confirm skill metadata with the user

At minimum, nail down:

| Field | Description | Example |
|---|---|---|
| `skill_id` | Globally unique id, lowercase + underscore | `home_lights_scene` |
| `summary` | One-sentence English summary used by LLM routing, <= 240 chars | `Set predefined RGB scenes (warm/cool/party) on the on-board WS2812 via the existing nm_cyd_c5_rgb script.` |
| `cap_groups` | Cap groups the skill needs; controls visibility on `activate_skill` | `["cap_lua"]` or `["cap_web_search","cap_time"]` |
| `triggers` | Sample user phrases (any language) that should fire it | `"warm scene", "客厅暖光"` |
| `steps` | Which caps to call and what arguments to pass | See template below |

If anything is missing, **ask** — do not guess.

### 2. Check the id does not collide

```
list_skill {}
```

Make sure `skill_id` is not present in the returned `skills[].id`. On collision, ask the user to rename, or go through `unregister_skill` to overwrite.

### 3. Draft the markdown

Use the following skeleton — it keeps skills consistent and human-readable **and forces the agent to actually call the tools instead of only replying with text**:

```markdown
# <Skill Title>

<one-sentence positioning, aligned with summary>

## When to use
<trigger conditions, keywords, situations where it should NOT fire>

## Execution discipline (MUST follow)
- This skill describes **mandatory tool calls**, not narration. You MUST emit the listed tool calls.
- Do **not** reply to the user with a confirmation sentence before the tool calls succeed. Replying without invoking the tools is a bug.
- After every tool call returns, base your final reply on the actual return value (ok / error). If a call fails, report the failure honestly — do not pretend it worked.

## Tool calls (in order)
1. `<cap_or_script_name>` with arguments `{...}` — purpose: <what hardware effect this produces>
2. `<next_call>` with arguments `{...}` — purpose: ...

## Reply template
Only after **all** tool calls above have returned ok, reply with something like:
> <short user-facing sentence, may include emoji>

## Example
User: "<trigger phrase>"
Agent tool calls (in this exact order):
1. tool_call: `<cap_or_script_name>` args=`{...}` -> ok
2. tool_call: `<next_call>` args=`{...}` -> ok
Agent reply: "<final sentence>"
```

Guidelines:

- Keep it under 80 lines. If it grows beyond that, split into multiple skills.
- **Do not** inline large Lua / shell blobs. If the skill needs to run a script, use `lua_save_script` to persist it under `/fatfs/scripts/builtin/` and have the markdown only reference `run_lua_script script="<name>" args={...}`.
- Use absolute `/fatfs/...` paths or cap_files-relative paths like `skills/<id>.md`.
- Always write tool calls with **imperative verbs and explicit argument JSON** (`run_lua_script script="nm_cyd_c5_backlight" args={"percent":5}`). Never write them as English prose like "dim the backlight to 5%" — the agent will treat prose as narration and skip the call.
- Reply text and tool-call list must be in **separate sections**. Never collapse them into a single "do X and reply Y" sentence — that pattern caused the agent to skip the tool call in the past.
- **Do not include any `add_router_rule` / `update_router_rule` / `delete_router_rule` calls in the skill body** unless Hard rule 3 / 4 are both satisfied. For IM-text → action skills, the skill body lists `run_lua_script` calls and that is enough; the agent will execute them on each matching turn.
- If the user asks the skill to also handle a hardware/sensor event (e.g. button, GPIO, scheduler tick), branch the markdown into two sections: `## Tool calls (in order) — when triggered by IM text` and `## Tool calls (in order) — when triggered by <event_type>`. Only the latter section may rely on a router rule, and only after Hard rule 4 is satisfied.

### 4. Write the file

Persist via `write_file` (cap_files). The path is **relative** to the cap_files base (i.e. `/fatfs`):

```
write_file path="skills/<skill_id>.md" content="<full markdown text>"
```

> Use `<skill_id>.md` as the filename so id and file map 1:1.

After writing, sanity-check with `read_file path="skills/<skill_id>.md"` to confirm there is no truncation or escaping issue.

### 5. Register it in the catalog

```
register_skill skill_id="<skill_id>" file="<skill_id>.md" summary="<same as above>"
```

> The `file` field is relative to the `skills/` directory — **do not** prefix it with `skills/`.
> `register_skill` reloads the whole `skills_list.json` and verifies the file exists. If it returns non-OK, clean up the half-baked markdown with `delete_file path="skills/<skill_id>.md"` to avoid leftovers.

### 6. (Optional) Activate and demo once

```
activate_skill skill_ids=["<skill_id>"]
```

Then run the steps described by the new skill end-to-end and report the result back to the user as proof that it works. The agent may decide on its own whether to `deactivate_skill` afterwards — no need to force it.

### 7. Send the user a receipt

A short 1-2 sentence summary:
- new skill `id`, save path, cap dependencies;
- example trigger phrases;
- a hint like "Next time just say …… and I'll do it automatically."

## Failure handling

| Situation | Handling |
|---|---|
| `register_skill` returns `duplicate skill_id` | Ask the user to rename; do not unregister the existing one without consent |
| `write_file` fails (full FS / invalid path) | Tell the user; do **not** proceed to register |
| User's description is vague and steps are not stable | Refuse to create the skill and ask for clarification — skills must be **repeatable** |
| `skill_id` contains non-ASCII / uppercase / spaces | Auto-normalise to `snake_case` and confirm with the user |

## Example

User: "From now on, when I say 'good night', dim the screen and set the lights to 5%. Save this as a skill."

Agent call chain:

1. `list_skill {}` — confirm `night_mode` is not taken.
2. `write_file path="skills/night_mode.md" content="# Night Mode\n\nDim the on-board LCD when the user says good night.\n\n## When to use\nUser says 'good night' / '晚安' / 'going to bed'.\nDo NOT fire on 'good morning' or general greetings.\n\n## Execution discipline (MUST follow)\n- This skill mandates real tool calls. You MUST emit them.\n- Do not reply with a confirmation before the tool calls return ok.\n- If any call returns error, report the error and abort the remaining steps.\n\n## Tool calls (in order)\n1. run_lua_script script=\"nm_cyd_c5_backlight\" args={\"percent\":5} -- dim backlight to 5%\n2. run_lua_script script=\"nm_cyd_c5_screen\" args={\"mode\":\"clear\"} -- clear LCD\n\n## Reply template\nAfter both calls return ok, reply (one line, may include emoji):\n> Good night, dimmed for you. 🌙\n\n## Example\nUser: 'good night'\nAgent tool calls:\n1. tool_call: run_lua_script args={\"script\":\"nm_cyd_c5_backlight\",\"args\":{\"percent\":5}} -> ok\n2. tool_call: run_lua_script args={\"script\":\"nm_cyd_c5_screen\",\"args\":{\"mode\":\"clear\"}} -> ok\nAgent reply: 'Good night, dimmed for you. 🌙'\n"`
3. `register_skill skill_id="night_mode" file="night_mode.md" summary="When the user says good night / 晚安, MUST call run_lua_script nm_cyd_c5_backlight {percent:5} then nm_cyd_c5_screen {mode:clear}; only after both ok, reply 'Good night, dimmed for you.'"`
4. `activate_skill skill_ids=["night_mode"]` and actually run it once to prove the tool calls fire.
5. Reply: "Added skill `night_mode`. Next time you say 'good night' I'll dim the screen automatically."

## Fixing an existing skill that only replies with text

If a previously registered skill replies but never triggers hardware (a common failure mode), it almost always lacks the **Execution discipline** + **Tool calls** sections above. To fix:

1. `read_file path="skills/<broken_id>.md"` to see the current content.
2. `write_file path="skills/<broken_id>.md" content="<rewritten markdown using the skeleton in step 3>"` — overwrite in place.
3. (Optional) `unregister_skill` then `register_skill` only if the `summary` also needs to change; the markdown rewrite alone is enough when only the body is wrong.
4. `deactivate_skill skill_ids=["<broken_id>"]` then `activate_skill skill_ids=["<broken_id>"]` to reload the doc into the prompt.
5. Ask the user to retry the original trigger phrase and confirm tool calls now appear in the logs.
