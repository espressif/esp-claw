# Writing Lua-Backed Packaged Skills

Use this reference when creating or updating a packaged skill that includes Lua files.

## File Layout

Rules:

- Put every bundled Lua script under `skills/<skill_id>/scripts/*.lua`.
- Reference bundled scripts in `SKILL.md` with `{CUR_SKILL_DIR}/scripts/<file>.lua`.
- Do not expose a Lua file without a matching skill that describes the user intent, prerequisites, arguments, and reporting behavior.
- Do not write absolute paths, paths with `..`, backslash paths, or non-`.lua` files.
- Reuse one skill-local script path during iteration instead of creating `foo2.lua`, `foo3.lua`, and similar variants.

## Reference Read Strategy

For Lua authoring, debugging, or hardware control, gather likely needed references before writing code.
Prefer several consecutive `read_file` tool calls in the same step instead of reading one document, reasoning, then coming back for another.

- Always read `scripts/builtin/lib/arg_schema.md` and  `scripts/builtin/lib/lua_module_delay.md` when script args are involved.
- Read every Lua module doc needed by the task before code generation.
- Read the closest builtin test script source as the implementation pattern.
- Activate `builtin_lua_modules` and use its table to find the needed doc path.
- Batch-read module docs with `read_file("scripts/docs/<doc-file>")`.
- If output ends with `[read_file truncated: ...]`, treat it as incomplete and do not infer missing APIs.

Builtin patterns:

- Display scripts use `board_manager`, `display`, `delay`, `display.begin_frame`, `display.present`, `display.end_frame`, and `pcall(display.deinit)` cleanup.
- Long display animations or games should usually run async with `exclusive: "display"` and a stable `name`.
- Async scripts should print short progress lines; running job logs can be read later with `lua_get_async_job` or incrementally with `lua_tail_async_job`.
- Hardware scripts open resources in `run()`, close them in `cleanup()`, then wrap execution in `xpcall(run, debug.traceback)`.

## Lua File Pattern

Use this Lua file pattern for Lua-backed packaged skills:

```lua
local arg_schema = require("arg_schema")

local ARG_SCHEMA = {
  duration_ms = arg_schema.int({ default = 1000, min = 0 }),
  enabled = arg_schema.bool({ default = true }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

local function cleanup()
  -- Release opened hardware, files, network sessions, or other resources here.
end

local function run()
  -- Perform the action here using validated ctx values.
  print("done: enabled=" .. tostring(ctx.enabled) .. " duration_ms=" .. tostring(ctx.duration_ms))
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[skill_id] ERROR: " .. tostring(err))
  error(err)
end
```

## Authoring Rules

- Runtime exposes tool `args` as global `args`; it must be an object.
- IM-triggered runs may auto-inject `channel`, `chat_id`, and `session_id` into `args` when absent.
- Use `arg_schema` for typed defaults and validation when it can express the argument shape.
- If a skill-local script needs `arg_schema`, use `require("arg_schema")`; inline only helpers that are not in the configured search paths.
- Validate required values and unsafe strings before operating hardware, files, or network-backed capabilities.
- Use `print(...)` for concise diagnostics because execution captures print output.
- For async scripts, keep each `print(...)` line compact and useful. The async runner keeps a bounded per-job log buffer, so high-volume logs will overwrite older lines.
- Keep user-facing text simple and predictable.
- Do not assume extra Lua modules exist. Only `require` modules documented by `builtin_lua_modules` and the docs you read.

## Quality Rules

- Never busy-wait. Use `delay.delay_ms(ms)` inside loops.
- Convert GPIOs, coordinates, sizes, counters, and display font sizes to integers with `math.floor(...)`, integer division, or schema normalization.
- For hardware resources, open them in `run()`, release them in cleanup code, and wrap execution with `xpcall(run, debug.traceback)`.
- For display scripts, do not deinitialize immediately after `present()` if the user expects the image to remain visible. Use async jobs for held displays.
