# Rust workspace roadmap

Forward-looking work for the `framework/runtime` workspace. Items here are
**intentional boundaries** already present in the code (documented and lightly
exercised) that a future change grows into — not dead code to delete.

## Async, fair-scheduling tool runner

`ToolRunner` (`claw-tool/src/runner.rs`) has two permanent execution surfaces:
`run_one()` and `run_one_async()`. Keep both. The sync surface is intentional, not
compatibility-only code; immediate and C-backed tools should be able to stay
sync. The async surface is the agent runtime path and drives both async handlers
and sync handlers without blocking the main agent executor.

Remaining work here is not "make claw-tool async"; it is fair scheduling and
concurrent execution on top of the existing async runner.

### 1. Per-call automatic retry budget

- **Boundary:** `ToolHandler::invoke` and `AsyncToolHandler::invoke_async` return
  `Result<ToolOutput, ToolInvokeError>`,
  where `ToolInvokeError` carries a `ToolRetryCount`. A handler can ask the
  runtime to re-invoke the *same* call after a transient failure via
  `tool_invoke_err_with_retries(error, ToolRetryCount::extra(n))`.
- **Current behavior:** sync and async retry loops re-invoke immediately, with
  **no backoff and no preemption between attempts**, and re-run argument parsing
  + schema validation each attempt.
- **Planned:** move retry into the scheduler with
  - exponential (or policy-driven) **backoff** between attempts,
  - a **preemption/cancellation check** between attempts (today a multi-retry
    call cannot be interrupted at an `iteration_loop` checkpoint),
  - retries restricted to genuinely transient failures (`InvokeRejected`-class),
    not deterministic validation errors, and
  - skipping the redundant re-validation of unchanged arguments.
- **Until then:** real tools should leave the budget at `ToolRetryCount::none()`
  (the default via `From<ToolError>`); only opt into a small budget for a
  verified transient failure.

### 2. Concurrent (fair-scheduled) execution

- **Boundary:** `ToolHandler::concurrent()` and `AsyncToolHandler::concurrent()`
  declare whether a tool is safe to run concurrently (no observable side effects
  that could interleave badly), surfaced via `ToolSet::concurrent` and
  `ToolRunner::is_concurrent` (today `#[allow(dead_code)]`).
- **Planned:** a fair-scheduling async runner awaits side-effect-free
  `concurrent` calls together while serializing mutating ones, keeping the
  *classify → gate → execute* shape so callers (the iteration loop) do not change
  when concurrency lands. "Fair scheduling" = bound how much wall-clock any one
  batch of tool calls can consume so a slow/greedy tool cannot starve the others.
- **Until then:** nothing consults `concurrent()`; every call runs in order.
