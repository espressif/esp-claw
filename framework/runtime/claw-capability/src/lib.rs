//! `claw-capability` — the capability adapter.
//!
//! A *capability* is the single outward-facing vocabulary callers speak.
//! Internally there is no such thing: a capability decomposes into a **role** —
//! a [`Tool`](CapabilityRole::Tool) (a [`claw_tool::Tool`]) or a
//! [`Channel`](CapabilityRole::Channel) (a message transport) — plus an
//! orthogonal, optional [`Lifecycle`] (init/start/stop). A capability with
//! [`no role`](CapabilityRole::None) exists purely for its lifecycle (e.g. an
//! MCP server toggled by enable/disable).
//!
//! This crate is an **adapter and classifier**, not a runtime. It owns
//! capability identity and the lifecycle state machine, and hands out the
//! internal representations — [`claw_tool::Tool`]s and [`ChannelAdapter`]s —
//! that the rest of the stack consumes. It deliberately holds **no** tool
//! dispatch, schema rendering, LLM visibility, or session logic: *which* tools
//! an agent sees, and *when*, is decided by `claw-core` (composing per-agent
//! `ToolSet`s with skills / soft-hide), never re-entering this layer.
//!
//! Rust capabilities can expose async model-callable tools by passing a
//! [`Tool::new_async`] tool to [`Capability::from_tool`]. C-backed descriptors
//! stay on the synchronous callback ABI; the async surface is for Rust
//! implementations.
//!
//! Two optional, dependency-injected concerns round out the registry:
//! [`CapabilityObserver`] receives a [`CapabilityChange`] after every mutation
//! (so a consumer can rebuild wholesale — no soft/incremental patching), and
//! [`CapabilityStateStore`] persists the enable/disable deny-list across reboots.
//! Both default to absent, so a plain [`Registry::new`] stays purely in-memory.
//!
//! ## Tool-authoring vocabulary
//!
//! A caller defines a tool by implementing [`ToolHandler`] (sync) or
//! [`AsyncToolHandler`] (async), wrapping it with [`Tool::new`] /
//! [`Tool::new_async`], and handing that to [`Capability::from_tool`]. The handler
//! traits, [`Tool`], their argument/result types ([`ToolInvocation`],
//! [`ToolOutput`], [`ToolInvokeError`], …) and the [`tool_metadata!`] macro are
//! re-exported here, so a caller depends on `claw-capability` alone and never
//! names the underlying tool framework.
//!
//! # Example
//!
//! ```
//! use claw_capability::{Capability, Registry, Tool, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};
//!
//! struct Clock;
//! impl ToolHandler for Clock {
//!     fn name(&self) -> &str { "get_time" }
//!     fn schema(&self) -> &str {
//!         r#"{"type":"function","function":{"name":"get_time"}}"#
//!     }
//!     fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
//!         Ok(ToolOutput { output: "now".into(), ok: true })
//!     }
//! }
//!
//! let registry = Registry::new();
//! registry
//!     .register(Capability::from_tool(Tool::new(Clock)))
//!     .expect("register clock");
//! registry.start_all().expect("start");
//!
//! // `claw-core` would assemble these into a per-agent ToolSet.
//! assert_eq!(registry.tools().len(), 1);
//! ```

mod capability;
mod channel;
mod error;
mod lifecycle;
mod observer;
mod registry;
mod state_store;

pub use capability::{Capability, CapabilityGroup, CapabilityRole};
pub use channel::{ChannelAdapter, ChannelFuture, ChannelRuntime, InboundMessage, OutboundMessage};
pub use error::CapabilityError;
pub use lifecycle::{CapabilityState, Lifecycle};
pub use observer::{CapabilityChange, CapabilityObserver};
pub use registry::Registry;
pub use state_store::{CapabilityStateStore, FsCapabilityStateStore};

// The tool-authoring vocabulary a caller needs to build a tool capability,
// re-exported so downstream callers depend on `claw-capability` alone: the handler
// traits plus `Tool` (built via `Tool::new` / `Tool::new_async` and passed to
// `Capability::from_tool`). `ToolSet` / `ToolRunner` stay an internal seam used by
// `claw-core`.
pub use claw_tool::{
    tool_metadata, AsyncToolHandler, Tool, ToolError, ToolFuture, ToolHandler, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolRetryCount,
};

// One-time initializer for the process-wide async tool executor. The framework
// wiring layer (`claw-agent`'s `AgentSystem`) calls this internally so callers
// never touch global tool state; re-exported here so that layer depends on
// `claw-capability` alone rather than the underlying tool crate. The executor
// *implementation* still lives in `claw-tool`.
pub use claw_tool::init_tool_executor;
