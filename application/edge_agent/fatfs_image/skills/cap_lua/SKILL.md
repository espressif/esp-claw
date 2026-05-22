---
{
  "name": "cap_lua",
  "description": "Discover, read, write, run, inspect, and stop Lua scripts, including async Lua jobs and safe authoring patterns.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Lua Script Operations

Use this skill for Lua scripts: discover scripts, read module docs, write scripts, run scripts, and manage async jobs.

## Path Rules

There are two path namespaces. Do not mix them:

- Lua tool paths: used by `lua_list_scripts`, `lua_run_script`, `lua_run_script_async`, and `lua_write_script`
- `read_file` file paths: used by `read_file`

Rules:

- `builtin/test/` is for shipped examples, tests, and demos.
- `builtin/lib/` is for reusable Lua libraries. Read the matching `.md` guide before using a library.
- `docs/` is for module API docs.
- `temp/` is for iteration. `user/` is for reusable scripts.
- Treat `builtin/` as read-only unless the user explicitly asks to change it.
- Lua tools use `builtin/...`, `temp/...`, and `user/...`.
- `lua_list_scripts` returns Lua tool paths.
- `lua_run_script`, `lua_run_script_async`, and `lua_write_script` accept Lua tool paths.
- Valid writable locations are `temp/*.lua` and `user/*.lua`.
- `read_file` uses file paths, not Lua tool paths.
- To read a script or library returned by `lua_list_scripts`, add exactly one leading `scripts/`.
- To read a skill-bundled Lua file, use `skills/<skill_id>/scripts/...`.
- Never call `read_file("scripts/scripts/...")`.
- Never pass a bare Lua tool path directly to `read_file`.

Examples:

| Intent | Correct path |
| --- | --- |
| `lua_list_scripts` result | `builtin/test/abc.lua` |
| `lua_run_script` path | `builtin/test/abc.lua` |
| `lua_run_script_async` path | `builtin/test/abc.lua` |
| `lua_write_script` path | `temp/abc.lua` or `user/abc.lua` |
| `read_file` for listed script | `scripts/builtin/test/abc.lua` |
| `read_file` for library source | `scripts/builtin/lib/arg_schema.lua` |
| `read_file` for library guide | `scripts/builtin/lib/arg_schema.md` |
| `read_file` for module doc | `scripts/docs/lua_module_display.md` |
| `read_file` for skill script | `skills/skill_id/scripts/abc.lua` |

## Documentation Read Strategy

For Lua authoring, debugging, or hardware control, gather likely needed references before writing or running code.
Prefer several consecutive `read_file` tool calls in the same step instead of reading one document, reasoning, then coming back for another.

- Always read `scripts/builtin/lib/arg_schema.md` when script args are involved.
- Read every Lua module doc needed by the task before code generation.
- Read the closest builtin test script source as the implementation pattern.
- Keep each `read_file` call to one path; issue multiple calls when several documents are needed.
- If output ends with `[read_file truncated: ...]`, treat it as incomplete and do not infer missing APIs.

## Discover Scripts

Call `lua_list_scripts` before reading, editing, or running unless the exact path is already confirmed in the current turn.

`lua_list_scripts` input:

- `prefix`: optional Lua tool path prefix such as `builtin/`, `temp/`, or `user/`. It must not be absolute and must not contain `..`.
- `keyword`: optional case-insensitive substring filter on the relative path.

Reading source:

- Convert Lua tool paths to `read_file` paths using the table above.

## Use Builtin References

Before choosing modules, APIs, or arguments for new code:

- Prefer task-specific skills for user-facing actions.
- Use `builtin/test/` for module or hardware validation, including advanced demos, and `builtin/lib/` only as reusable source to read or require.
- Read the `.lua` source only when needed.
- Activate `builtin_lua_modules` and use its table to find the needed doc path.
- Batch-read module docs with `read_file("scripts/docs/<doc-file>")`.
- Read the closest builtin script source and reuse its patterns.

Builtin patterns:

- Display scripts use `board_manager`, `display`, `delay`, `display.begin_frame`, `display.present`, `display.end_frame`, and `pcall(display.deinit)` cleanup.
- Long display animations or games should usually run async with `exclusive: "display"` and a stable `name`.
- Hardware scripts open resources in `run()`, close them in `cleanup()`, then wrap execution in `xpcall(run, debug.traceback)`.

## Write Scripts

Use `lua_write_script` for all Lua writes. Do not route script writes through shell or console commands.

Rules:

- `lua_write_script` requires `path` and `content`; `overwrite` defaults to `true`.
- Write first drafts and iterations to a stable Lua tool path under `temp/*.lua`.
- Move or rewrite to `user/*.lua` only after the script is confirmed useful.
- Do not write absolute paths, paths with `..`, backslash paths, or non-`.lua` files.
- Parent directories are created automatically.
- Reuse one path during iteration instead of creating `foo2.lua`, `foo3.lua`, and similar variants.

Authoring rules:

- Runtime exposes tool `args` as global `args`; it must be an object.
- IM-triggered runs may auto-inject `channel`, `chat_id`, and `session_id` into `args` when absent.
- Use `print(...)` for diagnostics because execution captures print output.
- Do not assume extra Lua modules exist. Only `require` modules documented by `builtin_lua_modules` and the docs you read.
- If a `temp/` or `user/` script needs `arg_schema`, use `require("arg_schema")`; inline only helpers that are not in the configured search paths.
- Keep user-facing text simple and predictable.

Quality rules:

- Never busy-wait. Use `delay.delay_ms(ms)` inside loops.
- Convert GPIOs, coordinates, sizes, counters, and display font sizes to integers with `math.floor(...)`, integer division, or schema normalization.
- For hardware resources, open them in `run()`, release them in `cleanup()`, and wrap execution in `xpcall(run, debug.traceback)`.
- For display scripts, do not deinitialize immediately after `present()` if the user expects the image to remain visible. Use async jobs for held displays.

Avoid busy-waits, blocking loops without delay, missing cleanup, float coordinates for integer APIs, and undocumented modules such as `socket`.

## Run Scripts

Use `lua_run_script` for short one-shot scripts where output is needed in the current turn.

- Input: `path`, optional object `args`, and optional positive `timeout_ms`. Omitted or `0` timeout means `60000` ms.
- `path` must be `builtin/...`, `temp/...`, `user/...`, or an absolute path from a skill's instructions.
- `args` is a JSON object keyed by parameter name, for example `"args":{"enabled":true}`.
- `args` must be an object.
- Lua reads parameters from global `args`.
- No output returns `Lua script completed with no output.`.
- Long output ends with `[output truncated]`. On failure, error text is appended after captured output.

Use `lua_run_script_async` for loops, animations, monitors, games, display holds, and other long-running work.

Async behavior:

- Input: `path`, optional object `args`, `timeout_ms`, `name`, `exclusive`, and `replace`.
- The async `path` uses the same forms as `lua_run_script`.
- `timeout_ms` `0` or omitted means run until cancelled. Omitted `name` defaults to the script basename.
- At most 4 async Lua jobs can run concurrently.
- Active jobs with the same `name` cannot coexist.
- Active jobs with the same `exclusive` group cannot coexist.
- If `replace` is omitted or `false`, a conflict returns an error.
- If `replace=true`, the conflicting job is stopped first. If that does not complete in time, takeover fails.

- Always set `name` for long-running scripts.
- Set `exclusive` when the job owns singleton hardware such as display or audio.
- Use `replace:true` only when the user explicitly wants to switch jobs.

## Inspect And Stop Async Jobs

Use `lua_list_async_jobs` first, then `lua_get_async_job` when details are needed.

- Status values are `queued`, `running`, `done`, `failed`, `timeout`, and `stopped`.
- `lua_list_async_jobs` accepts optional `status`, supporting all statuses plus `all`.
- `lua_get_async_job` can query by `job_id` or `name`.
- Name lookup only matches active jobs. For finished jobs, prefer `job_id`.
- `lua_get_async_job` returns `job_id`, `name`, `status`, `exclusive`, `runtime_s`, `path`, `args`, and `summary`.

Stopping rules:

- If the user asks to stop, cancel, close, clear, or switch an async script, call the stop tool or run the replacement with `replace:true` in the same turn.
- Do not claim a job is stopped or switched based only on context.
- `lua_stop_async_job` input is `job_id` or `name`, with optional `wait_ms`.
- `lua_stop_all_async_jobs` input is optional `exclusive` and optional `wait_ms`.
- If `wait_ms` is omitted or `0`, the default wait is `2000` ms.
- If stop wait times out, do not assume the job is fully stopped yet.

## IM And Events From Lua

- For direct user replies, images, or files that do not depend on Lua runtime state, prefer calling the corresponding IM capability outside Lua.
- For router trigger events from Lua, read `builtin/test/llm_analyze_trigger.lua` and the event publisher docs before emitting a table event.

## Recommended Flow

1. Classify the task as run existing, write/adapt, async inspect/stop, or IM/event integration.
2. Activate `builtin_lua_modules` before writing or adapting code.
3. Call `lua_list_scripts`, usually with `prefix: "builtin/"` first, then read the closest builtin source.
4. Read only the needed module docs from `scripts/docs/*.md`.
5. If an existing builtin script satisfies the request, run it directly with documented args.
6. If adaptation is needed, write one stable `temp/*.lua`, run it, inspect output, then iterate on the same path.
7. Use sync execution for short tasks and async execution with `name` and `exclusive` for long-running or hardware-owning tasks.
8. Stop or replace async jobs with the relevant Lua tool before saying they are stopped or switched.
