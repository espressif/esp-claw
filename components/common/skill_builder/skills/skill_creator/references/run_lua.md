# Running Lua-Backed Packaged Skills

Use this reference when creating or updating a packaged skill that needs to document how its Lua script is run.

## Run Scripts

Prefer running Lua through a skill-owned script path. For user-facing behavior, the skill must be activated first and its instructions must provide the exact script path and args contract.

- Input: `path`, optional object `args`, and optional positive `timeout_ms`. Omitted or `0` timeout means `60000` ms.
- `path` must end in `.lua` and must not contain `..`. Use a bundled skill-local path such as `{CUR_SKILL_DIR}/scripts/action.lua`, or a path returned by Lua script discovery tools.
- For user-facing work, `path` should follow the active skill's documented script path; `builtin/...` reference scripts are allowed for inspection and demos only.
- `args` is a JSON object keyed by parameter name, for example `"args":{"enabled":true}`. Lua reads it from global `args`.
- No output returns `Lua script completed with no output.`.
- Long output ends with `[output truncated]`. On failure, error text is appended after captured output.

Use `lua_run_script` for short one-shot skill scripts where output is needed in the current turn.

Use `lua_run_script_async` for skill-owned loops, animations, monitors, games, display holds, and other long-running work.

## Async Behavior

- Input: `path`, optional object `args`, `timeout_ms`, `name`, `exclusive`, `replace`, and `log_bytes`.
- `timeout_ms` `0` or omitted means run until cancelled. Omitted `name` defaults to the script basename.
- `log_bytes` controls the per-job live log buffer. Omitted means `4096`; valid range is `1024` to `16384`.
- At most 4 async Lua jobs can run concurrently.
- Active jobs with the same `name` cannot coexist.
- Active jobs with the same `exclusive` group cannot coexist.
- If `replace` is omitted or `false`, a conflict returns an error.
- If `replace=true`, the conflicting job is stopped first. If that does not complete in time, takeover fails.
- `print(...)` output is captured into the live job log while the script is running and into `summary` when the script finishes.

Rules:

- Always set `name` for long-running scripts.
- Set `exclusive` when the job owns singleton hardware such as display or audio.
- Use `replace:true` only when the user explicitly wants to switch jobs.
- Use larger `log_bytes` only for scripts with useful multi-step progress; avoid high-volume logs.

## Async Logs

Use `lua_get_async_job` for a status snapshot. It returns the job metadata, final `summary` when available, and a `recent_log` tail.

Use `lua_tail_async_job` for incremental logs:

- Input: `job_id` or `name`, optional `since_seq`, optional `max_bytes`.
- First call may omit `since_seq`; read `log_next_seq` from the response.
- Later calls should pass the previous `log_next_seq` as `since_seq` to read only new text.
- If `log_truncated=true`, older log text was overwritten by the job's bounded buffer.

## Skill Instructions Checklist

When documenting a Lua-backed skill, include:

- The exact script path, usually `{CUR_SKILL_DIR}/scripts/<name>.lua`.
- The args schema and examples for common tool call inputs.
- Whether the script should run with `lua_run_script` or `lua_run_script_async`.
- Any `timeout_ms`, `name`, `exclusive`, `replace`, or `log_bytes` policy.
- The expected success output and how errors should be reported.
- For async scripts, how to check status and logs with `lua_get_async_job` or `lua_tail_async_job`.
