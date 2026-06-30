# Rust workspace roadmap

Forward-looking work for the `framework/runtime` workspace. Items here are
**intentional seams** already present in the code (documented and lightly
exercised) that a future change grows into — not dead code to delete.

## Async, fair-scheduling tool runner

Today `ToolRunner` (`claw-tool/src/runner.rs`) executes one tool call at a time,
synchronously, in the model's order. Two seams are in place for a future async
runner; keep them until that runner lands.

### 1. Per-call automatic retry budget

- **Seam:** `ToolHandler::invoke` returns `Result<ToolOutput, ToolInvokeError>`,
  where `ToolInvokeError` carries a `ToolRetryCount`. A handler can ask the
  runtime to re-invoke the *same* call after a transient failure via
  `tool_invoke_err_with_retries(error, ToolRetryCount::extra(n))`.
- **Current behavior:** `invoke_with_retries` re-invokes immediately, in a tight
  loop, with **no backoff and no preemption between attempts**, and re-runs
  argument parsing + schema validation each attempt.
- **Planned:** when the async runner lands, move retry into it with
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

- **Seam:** `ToolHandler::concurrent()` declares whether a tool is safe to run
  concurrently (no observable side effects that could interleave badly), surfaced
  via `ToolSet::concurrent` and `ToolRunner::is_concurrent` (today `#[allow(dead_code)]`).
- **Planned:** a fair-scheduling async runner awaits side-effect-free
  `concurrent` calls together while serializing mutating ones, keeping the
  *classify → gate → execute* shape so callers (the iteration loop) do not change
  when concurrency lands. "Fair scheduling" = bound how much wall-clock any one
  batch of tool calls can consume so a slow/greedy tool cannot starve the others.
- **Until then:** nothing consults `concurrent()`; every call runs in order.
