# claw-tool

The tool framework for the ESP-Claw agent framework: define a model-callable
*tool* once, pool tools in a registry, carve per-agent sets out of that pool,
gate and execute calls, and enforce the on-disk tool contract at build time.

`claw-tool` sits below `claw-core` (and beside `claw-permission`): it knows
nothing about agents or the orchestrator, only about *tools*. The same code runs
on-device and in host tests.

## Sync and async are both intentional

`claw-tool` deliberately exposes two first-class execution surfaces:

- **Sync:** `ToolHandler`, `Tool::new`, `Tool::invoke`,
  `ToolSet::invoke`, and `ToolRunner::run_one`.
- **Async:** `AsyncToolHandler`, `Tool::new_async`, `Tool::invoke_async`,
  `ToolSet::invoke_async`, and `ToolRunner::run_one_async`.

Do not treat the sync surface as migration leftover or compatibility-only code.
Some tools are naturally immediate or C-backed and should stay sync. Rust tools
that await I/O or other cooperative work should implement the async surface. The
agent runtime drives tools through `run_one_async`; sync handlers are still valid
there because `Tool::invoke_async` moves the handler body onto the fixed tool
executor instead of blocking the main agent executor.

## The on-disk tool contract

A baked tool's metadata lives in `resources/tools/<name>/`, holding **exactly**:

```text
resources/tools/read_file/
├── schema.json   # one {"type":"function", "function": {"name":"read_file", ...}} object
└── usage.md      # soft-tools prompt prose (blank file ⇒ no usage)
```

The directory name must equal the schema's `function.name`. The
[`tool_metadata!`] macro reads these files at runtime, and `bake::validate_tools_dir`
(called from a dependent crate's build script) enforces the layout at build time,
so the runtime and build-time halves of the contract can never drift.

Defining a baked tool is then just the `invoke` body:

```rust,ignore
struct ReadFileTool;

impl ToolHandler for ReadFileTool {
    tool_metadata!("read_file"); // generates name() / schema() / usage()

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolError> {
        // ...read the file named in call.arguments_json...
        Ok(ToolOutput { output: "...".into(), ok: true })
    }
}
```

Tools can also be **runtime-registered** with dynamic (owned) names — handy for
MCP or other tools discovered at run time. Both kinds live in the same registry
and are selected uniformly.

## Public API

Re-exported from the crate root:

| Type | Role |
|------|------|
| `ToolHandler` | Sync trait for one model-callable tool — `name()`, `schema()` (OpenAI function JSON), optional `usage()`, optional `concurrent()` / `classify()`, and `invoke()`. |
| `AsyncToolHandler` / `ToolFuture` | Async Rust trait for one model-callable tool. It mirrors `ToolHandler` metadata/classification and implements `invoke_async()`. |
| `tool_metadata!` | Macro: generate `name`/`schema`/`usage` from a baked `resources/tools/<name>/` directory. |
| `Tool` | A cheap-to-clone (`Arc`-backed) handler value wrapping either a sync or async implementation. |
| `init_tool_executor` | Initializes the fixed async tool executor with a caller-supplied `T: ClawThread` backend. Required before driving `Tool::invoke_async` / `ToolRunner::run_one_async`. |
| `ToolRegistry` | A pool of every known tool, keyed by name. `register()` / `register_as()` / `unregister()`, then `select()` / `select_all()` / `group()` to build sets. |
| `ToolGroup` | A named bundle of tools; the name tags provenance (it does *not* namespace dispatch — names stay flat and globally unique). |
| `ToolSet` | The per-agent aggregate the iteration loop consumes: precomputed combined `schemas_json()`, flat O(1) `invoke()` / `invoke_async()`, plus the soft-tools state it owns. |
| `AllowedTools` | The soft-hide phase allow-set: which tools may *execute* this phase. |
| `BlockPolicy` / `ToolBlockVerdict` | The soft-hide "retry then fail" policy and the verdict it returns. The default budget is expressed by `BlockPolicy::default()`. |
| `ToolRunner` | Per-call execution boundary: soft-hide gating → permission gating → dispatch via `run_one()` or `run_one_async()`, returning a neutral `CallOutcome`. |
| `ToolGate` / `PermissionGate` | The permission boundary the runner consults; `PermissionGate` is the policy + grant-store implementation the agent installs. |
| `CallOutcome` / `ApprovalNeeded` | The runner's per-call verdict, and what an `Ask` decision needs the agent to resolve. |
| `ToolError` / `ToolSetError` | Failure enums for invocation and set assembly. |

### Soft tools (phase gating)

The full schema in `schemas_json()` is **always** sent to the model, so the
cached `tools` prompt prefix never moves. What changes per phase is which of
those tools may actually *run*: `ToolSet::set_active_tools(AllowedTools)`
restricts execution, `is_allowed(name)` answers the runner, and two prompt
surfaces stay together on the set:

- `tool_context()` — the **static** per-tool usage block (name-ordered, stable
  bytes), belongs in the cached system prefix.
- `extra_tool_context()` — the **dynamic** one-line phase note naming the
  currently-active tools, belongs in the ephemeral request tail.

If the model keeps calling a blocked tool, `record_round(&blocked)` counts the
consecutive blocked rounds and returns `ToolBlockVerdict::Exhausted` once they
exceed `block_retries`, so the agent can end the task.

### Design notes

- **Wire surfaces are precomputed and name-ordered.** `schemas_json` and
  `tool_context` are rebuilt only when tool membership changes; `extra_tool_context`
  only when the active set changes. Per-request access is a free borrow, and the
  byte-stable ordering keeps the server-side prompt cache warm across restarts.
- **Flat, globally-unique names.** Dispatch is by name across all groups; a
  duplicate name is a hard `ToolSetError::DuplicateToolName` at assembly.
- **The sync and async runners are both permanent API.** `run_one()` exists for
  synchronous callers and immediate/C-backed tools; `run_one_async()` is the
  agent runtime path. They share the same classify → gate → execute semantics
  and both turn refusals (soft-hide / deny / ask) into a `CallOutcome` rather
  than an error.

## Examples

Runnable on the host:

```bash
cargo run --example registry       --target x86_64-unknown-linux-gnu
cargo run --example soft_tools     --target x86_64-unknown-linux-gnu
cargo run --example run_with_gate  --target x86_64-unknown-linux-gnu
```

## Where it fits

`claw-tool` is a pure-Rust core crate depending only on `claw-permission`. In the
firmware, `claw_core` builds a `ToolRegistry`, selects each agent's `ToolSet` from
it, installs a `PermissionGate`, and drives the `ToolRunner` from the iteration
loop; `claw-context` places the `tool_context` / `extra_tool_context` strings into
the prompt.
