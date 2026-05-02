# Lua Script Discovery

Use this skill when the user wants to find existing Lua scripts, filter scripts by path, or prepare to read a script's source.

## Core Constraints

- Prefer finding and reusing an existing script before proposing a new one. Create a new script only when no existing script can satisfy the requirement.
- `lua_list_scripts` returns paths relative to the Lua script root.
- Built-in scripts live under `builtin/`. User-authored scripts should live under `temp/` or `user/`.
- Relative paths must not be absolute and must not contain `..`.
- If you need to read source afterwards, `read_file` must use `scripts/<relative_path>`, not the bare relative path.

## `lua_list_scripts`

Purpose: list relative paths for all managed Lua scripts.

Supported parameters:

- `prefix`: optional relative-path prefix filter.
- `keyword`: optional case-insensitive substring match on the relative path.

Limits:

- `prefix` must be a relative path. It must not be absolute and must not contain `..`.

Return behavior:

- On success, returns relative paths separated by newlines.
- When nothing matches, returns `(no Lua scripts found)`.

## Reading Source

- `lua_list_scripts` may return paths such as `builtin/button_demo.lua`, `temp/demo.lua`, or `user/demo.lua`.
- To read the source with `read_file`, use `scripts/builtin/button_demo.lua`, `scripts/temp/demo.lua`, or `scripts/user/demo.lua`.
- Do not pass the raw `lua_list_scripts` result directly to `read_file`.

## Recommended Flow

1. Call `lua_list_scripts` first, with `prefix` or `keyword` when needed.
2. If an existing script is close to the user's requirement, reuse that path or source instead of creating a new file.
3. After finding the target, use `read_file("scripts/<relative_path>")` if you need the source.
4. Switch to `cap_lua_edit` if the next step is editing.
5. Switch to `cap_lua_run` if the next step is execution or async job management.
