# Lua Script Execution

Use this skill when the user wants to inspect existing Lua scripts, run a script, inspect async jobs, or stop async jobs.

## Core Constraints

- Call the direct `cap_lua` capability entrypoints.
- `path` must be a relative `.lua` path.
- If the user asks to stop, cancel, close, or clear an async script, you must actually call `lua_stop_async_job`, `lua_stop_all_async_jobs`, or `lua_run_script_async` with `replace:true` in the same turn. Do not claim the job is stopped based only on context.
- Do not deactivate this skill while async Lua jobs are still running, or you lose the stop/list entrypoints needed to manage them.

## Before Execution

- Use `cap_lua_list` and call `lua_list_scripts` first to confirm the target script path.
- Prefer an existing script when it already matches or is close to the requested behavior. Only create a new script when reuse is insufficient.
- If you need the source first, follow the `cap_lua_list` rule and use `read_file("scripts/<relative_path>")`.
- Expect shipped demos and built-in examples to appear under `builtin/`.

## `lua_run_script`

Use this for short, one-shot work when output is needed in the current turn.

Input:

- `path`: required.
- `args`: optional, must be an object or array.
- `timeout_ms`: optional; when provided it must be a positive integer.

Implementation behavior:

- If `timeout_ms` is omitted or `0`, sync execution uses the default timeout of `60000` ms.
- Lua reads the parameters from the global `args`.
- For IM-triggered runs, the firmware may auto-inject `channel`, `chat_id`, and `session_id` into `args` when they are absent.
- `print(...)` output is captured.
- When there is no output, the result is `Lua script completed with no output.`.
- When output is too long, `[output truncated]` is appended at the end.
- On execution failure, the error text is appended after any captured output. For the console wrapper, this often means only the last non-empty error line is shown.

## `lua_run_script_async`

Use this for loops, animations, monitors, watchers, and other long-running tasks.

Input:

- `path`: required.
- `args`: optional, must be an object or array.
- `timeout_ms`: optional, must be a non-negative integer.
- `name`: optional logical job name.
- `exclusive`: optional mutex/resource-group name such as `display`.
- `replace`: optional boolean.

Implementation behavior:

- `timeout_ms=0` means run until cancelled, which is also the default behavior.
- If `name` is omitted, the runner uses the script basename as the job name.
- The async runner verifies that the script file exists before queueing the job.
- The system can run at most 4 async Lua jobs concurrently and keeps 16 total job slots; old terminal jobs may be recycled.

## `name` / `exclusive` / `replace`

These fields define async takeover behavior.

- `name`: active jobs with the same name cannot coexist.
- `exclusive`: active jobs in the same exclusive group cannot coexist.
- If `replace` is omitted or `false`, a conflict returns an error immediately.
- If `replace=true`, the runner first asks the conflicting job to stop and then submits the new job.
- Stopping the conflicting job is cooperative and waits 2000 ms by default; if that wait times out, takeover fails.

Recommended practice:

- Always set `name` for long-running scripts.
- Set `exclusive` when the job owns a singleton resource such as display or audio.
- Use `replace:true` only when the user explicitly wants to switch over to the new job.

## Async Job Inspection

Use:

- `lua_list_async_jobs`
- `lua_get_async_job`

Status values:

- `queued`
- `running`
- `done`
- `failed`
- `timeout`
- `stopped`

Details:

- `lua_list_async_jobs` accepts an optional `status` filter and supports all statuses above plus `all`.
- `lua_get_async_job` can query by `job_id` or `name`.
- Name lookup only matches active jobs. For terminal jobs, prefer `job_id`.
- `lua_get_async_job` returns `job_id`, `name`, `status`, `exclusive`, `runtime_s`, `path`, `args`, and `summary`.

## Stopping Async Jobs

Use:

- `lua_stop_async_job`
- `lua_stop_all_async_jobs`

Input:

- `lua_stop_async_job`: pass `job_id` or `name`, with optional `wait_ms`.
- `lua_stop_all_async_jobs`: optional `exclusive` filter and optional `wait_ms`.

Implementation behavior:

- If `wait_ms` is omitted or `0`, the default wait is `2000` ms.
- Stopping is cooperative; the script observes cancellation through the Lua hook.
- A successful single stop usually returns `OK: stopped job <id> (status=stopped)`.
- If the target is already terminal, the tool returns `OK: job <id> already terminal ...`.
- If the wait times out, the tool returns `WARN: stop requested ... but task did not exit within ...`, which means stop was requested but the job may still be unwinding.
- `lua_stop_all_async_jobs` returns aggregate counts for stopped jobs and jobs still unwinding.

## Recommended Flow

1. Use `cap_lua_list` first to confirm the target script path.
2. Prefer running or adapting an existing script when one is already suitable. Create a new script only when reuse does not satisfy the request.
3. Use `lua_run_script` for short work and `lua_run_script_async` for long-running work.
4. For built-in examples, run them from `builtin/*.lua` when checking baseline behavior. For iterative changes, copy them to a stable `temp/*.lua` path and keep stable `name` / `exclusive`.
5. Inspect state with `lua_list_async_jobs` first, then use `lua_get_async_job` when you need details.
6. When the user asks to stop a job, call the stop tool first and then answer based on the returned result.
