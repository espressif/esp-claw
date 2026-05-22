# Lua Script Editing

Use this skill when the user wants to create, modify, or organize Lua scripts. The goal is to use `cap_lua` script storage correctly instead of routing through console commands.

## Core Constraints

- Use `lua_write_script` for writes. 
- Prefer reusing or adapting an existing script. Create a new script only when existing scripts cannot satisfy the requirement.
- `path` must be a relative `.lua` path such as `builtin/foo.lua`, `temp/foo.lua`, or `user/bar.lua`.
- Do not use absolute paths, do not include `..`, and do not write non-`.lua` files.
- `lua_write_script` creates parent directories automatically.
- `overwrite` defaults to `true`. Only pass `false` when the user explicitly wants create-only behavior.
- Scripts still being validated should stay under `temp/`. Move them to `user/` only after they are confirmed.
- Built-in scripts under `builtin/` should usually be treated as references. Prefer adapting them into `temp/` or `user/` instead of editing the built-in path in place.
- Do not assume extra Lua modules exist. Only `require` modules explicitly documented by the active `lua_module_*` skills.

## Recommended Flow

1. Use `cap_lua_list` and call `lua_list_scripts` first to see whether a close script already exists, especially under `builtin/`.
2. Reuse or adapt the closest existing script whenever possible. Create a new script only when existing scripts do not meet the requirement.
3. If you want to reuse an existing script, read its source with `read_file`. When the source comes from `builtin/`, usually rewrite it into `temp/` or `user/` instead of editing the built-in path.
4. Write new scripts under `temp/*.lua` first, then move them to `user/*.lua` after confirmation.
5. Reuse the same path during iteration instead of creating `foo2.lua`, `foo3.lua`, and so on.

## `lua_write_script`

Purpose: write or overwrite a Lua script.

Required parameters:

- `path`
- `content`

Optional parameters:

- `overwrite`

Implementation behavior:

- The script size limit is 16 KiB.
- On success, the tool returns `OK: wrote Lua script <path> (<bytes> bytes)`.
- Common failure reasons include invalid path, missing content, existing script with `overwrite=false`, storage preparation failure, or file write failure.

## Runtime-Facing Rules While Authoring

- The runtime exposes tool `args` to Lua as the global `args`.
- `args` must be an object or array. Do not construct other JSON types.
- When execution is triggered from an IM session, the firmware may inject `channel`, `chat_id`, and `session_id` into `args` if they are absent.
- JSON integers are preserved as Lua integers when possible, but GPIOs, coordinates, and counters should still be made explicitly integral.
- `print(...)` output is captured by the execution capability, so prefer `print` for diagnostics.

## Quality Rules To Keep

- Reuse before creating. If an existing script is close, modify it instead of starting from scratch.
- Never busy-wait for timing. Use `delay.delay_ms(ms)` inside loops.
- For hardware resources, open them in `run()`, release them in `cleanup()`, and wrap execution in `xpcall(run, debug.traceback)`.
- When an API expects integers, use `math.floor(...)` or another explicit conversion.
- If the script needs to send IM replies, prefer call the corresponding IM capability directly.
