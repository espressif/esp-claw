# claw-capability

The **capability adapter** for the ESP-Claw agent framework.

A *capability* is the single outward-facing vocabulary callers speak. Internally
there is no such thing: a capability decomposes into a **role** plus an
orthogonal, optional **lifecycle**.

- **Role** — what the capability exposes when used:
  - `Tool` — a model-callable tool. It *is* a [`claw_tool::Tool`]; this crate adds
    no dispatch, schema, or visibility logic of its own. Build the `Tool` with
    `Tool::new(...)` (sync handler) or `Tool::new_async(...)` (async handler) and
    hand it to `Capability::from_tool(...)`.
  - `Channel` — a bidirectional message channel adapter.
  - `None` — no invocation surface; the capability exists only for its lifecycle.
- **Lifecycle** — optional resource management, available to *any* role (a `Tool`
  may own a runtime, a `Channel` owns its transport, a lifecycle-only capability
  is an MCP server toggled by enable/disable). Two paired phases run as
  `init → (start → stop)* → deinit`: the one-time `init`/`deinit` pair brackets
  the per-activation `start`/`stop` pair.

This crate is an **adapter and classifier**, not a runtime. It owns capability
identity and the lifecycle state machine, and hands out the internal
representations — `claw_tool::Tool`s and `ChannelAdapter`s — that the rest of the
stack consumes. It holds **no** tool dispatch, schema rendering, LLM visibility,
or session logic: *which* tools an agent sees, and *when*, is decided by
`claw-core` (composing per-agent `ToolSet`s with skills / soft-hide), never
re-entering this layer.

## Public API

| Item | Role |
|------|------|
| `Registry` | Owns capability identity + lifecycle: `register` / `register_group`, `start_all` / `stop_all`, `enable_group` / `disable_group`, `unregister[_group]`, plus role-based access (`tools`, `channels`) and state queries. |
| `Capability` / `CapabilityRole` | One capability (id, role, optional lifecycle) and its role (`Tool` / `Channel` / `None`). |
| `Capability::from_tool` | The single tool-capability constructor. Build the `Tool` with `Tool::new` / `Tool::new_async`; the tool-authoring vocabulary is re-exported here (incl. `Tool`), so callers depend on `claw-capability` alone. |
| `CapabilityGroup` | A registrable bundle of capabilities with an optional **shared** lifecycle (e.g. one runtime backing several tools). |
| `Lifecycle` | The orthogonal hooks on any capability or group: the one-time `init`/`deinit` pair and the per-activation `start`/`stop` pair (`init → (start → stop)* → deinit`). |
| `CapabilityState` | Lifecycle state: `Registered` / `Started` / `Disabled`. |
| `CapabilityObserver` / `CapabilityChange` | Observer notified (outside the registry lock) of `Registered` / `Unregistered` / `StateChanged` events; wired via `Registry::with_observer` / `add_observer`. |
| `CapabilityStateStore` / `FsCapabilityStateStore` | Persists the disabled-group deny-list across reboots; wired via `Registry::with_state_store`. |
| `ChannelAdapter` / `ChannelRuntime` / `InboundMessage` / `OutboundMessage` | Bidirectional message channel contract. |
| `CapabilityError` | Registration / lifecycle / persistence failure. |

## Example

```rust
use claw_capability::{Capability, Registry};
use claw_tool::{Tool, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};

struct Clock;
impl ToolHandler for Clock {
    fn name(&self) -> &str { "get_time" }
    fn schema(&self) -> &str { r#"{"type":"function","function":{"name":"get_time"}}"# }
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput { output: "now".into(), ok: true })
    }
}

let registry = Registry::new();
registry
    .register(Capability::from_tool(Tool::new(Clock)))
    .expect("register clock");
registry.start_all().expect("start");

// `claw-core` assembles these into a per-agent ToolSet.
assert_eq!(registry.tools().len(), 1);
```

## Where it fits

A pure-Rust core crate depending on `claw-tool` (for the `Tool` role) and
`thiserror`. It is fully host-testable and stays free of any platform or
transport details — those are injected through the `Lifecycle` and
`ChannelAdapter` traits.
