---
{
  "name": "lua_package_authoring",
  "description": "Author or update Lua scripts bundled in an explicitly requested runtime Skill or launcher App; use with the owning creator, not for running existing packages or adding native firmware capabilities.",
  "metadata": {
    "cap_groups": [
      "cap_files",
      "cap_lua"
    ]
  }
}
---

# Lua Package Authoring

Use this skill when `skill_creator` or `app_creator` needs new or updated bundled Lua scripts. The owning creator controls the package directory, manifest or Skill definition, update scope, removal, and publication. This skill must not call `publish_skill`, `remove_skill`, `publish_app`, or `remove_app`.

Lua can only compose modules and capabilities already compiled into the device. If the requested behavior needs a missing driver, native capability, Kconfig option, or firmware component, return that limitation to the owning creator instead of fabricating an API.

## Authoring Flow

1. Use the package kind, id, and absolute target directory resolved by the owning creator.
2. Activate `builtin_lua_modules` and read the documentation for every module the implementation will import.
3. Read a nearby bundled test only when it provides a relevant API or resource-lifecycle example.
4. Write the smallest complete set of scripts. A package may own one or more scripts; do not force unrelated actions into one file.
5. Read the completed files back and verify imports, documented APIs, arguments, package-local paths, cleanup, and expected output.
6. Return control to the owning creator for package verification and publication.

Do not infer undocumented functions from module names or test code from a different module.

## Package Paths

For a runtime Skill:

- Store scripts under `<SKILLS_ROOT>/<skill_id>/scripts/`.
- Reference them from `SKILL.md` with `{CUR_SKILL_DIR}/scripts/<file>.lua`.
- Do not reference files owned by an App or another Skill.
- Document synchronous or asynchronous tool execution as described below.

For a launcher App:

- Store scripts under `<APPS_ROOT>/<app_id>/scripts/`.
- Keep the `launcher.json` entry package-relative, such as `scripts/main.lua`.
- The runtime prepends the running script directory to `package.path`, so package-local Lua modules beside the entry script can be loaded with `require("module_name")`.
- Derive asset paths from the running script path, for example with `debug.getinfo(1, "S").source`, and then resolve the App directory. Do not hard-code `/system`, `/fatfs`, or an SD mount point.
- Do not reference files owned by a Skill or another App.

For writable user data, use `storage.get_root_dir()` with `storage.join_path(...)`. Reject unsafe user-controlled path fragments, addresses, GPIOs, sizes, and other resource identifiers before use. Reuse stable filenames during updates instead of creating numbered variants.

## Arguments And Results

- Runtime exposes the package arguments as the global `args` object.
- Use `arg_schema` when typed defaults or validation materially improve correctness.
- Keep output concise and predictable enough for the caller or launcher log to interpret.
- At a critical failure point, print a short contextual error and return or raise the actual error.
- Do not hide partial failure or report success before the operation completes.

## Resource Safety

- Acquire hardware, files, sockets, screens, and other resources as late as practical and release every acquired resource.
- Add protected execution and cleanup only when resources must be released on failure; do not add empty cleanup scaffolding.
- Never busy-wait. Use the documented delay module for timed loops.
- Keep async log volume bounded and make cancellation leave owned hardware in a safe state.

## Runtime Skill Execution

Use `lua_run_script` for bounded Skill work whose result is needed in the current turn. Its input contains the absolute bundled `.lua` path, an optional object `args`, and an optional positive `timeout_ms`; omitting the timeout uses the 60000 ms default. Do not pass zero.

Use `lua_run_script_async` for loops, monitors, animations, games, display holds, or Skill work that must outlive the current tool call. Give long-running jobs a stable `name`. Use `exclusive` for singleton hardware such as display or audio. Use `replace:true` only when the user explicitly wants to replace a conflicting job.

- An omitted or zero async `timeout_ms` runs until cancelled.
- At most four async Lua jobs run concurrently.
- Active jobs with the same `name` or non-empty `exclusive` group conflict.
- `log_bytes` defaults to 4096 and accepts 1024–16384. Increase it only for useful bounded progress logs.

Document only the execution details that affect each Skill script: path, arguments, execution mode, non-default job policy, success result, and meaningful failure output. Use `lua_get_async_job` for a status snapshot and `lua_tail_async_job` with the previous `log_next_seq` for incremental logs.

## Launcher App Execution

The launcher executes an App entry asynchronously with the manifest `args`. The App creator owns the launcher contract; do not add `lua_run_script` instructions to the App package. Do not start a long-lived App merely to validate its package.

## Verification And Failure Handling

Do not execute a script merely as a syntax check when it could mutate files, contact a network service, operate hardware, or take over the display. Run it only when validation is safe and within the user's request.

Report script errors accurately. Retry only when the failure is understood, the retry is safe, and the corrected input still matches the user's request. Do not silently change user-provided arguments, replace a running job, or execute another script as a fallback.
