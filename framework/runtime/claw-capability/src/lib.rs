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
//! Rust capabilities can expose async model-callable tools through
//! [`Capability::async_tool`]. C-backed descriptors stay on the synchronous
//! callback ABI; the async surface is for Rust implementations.
//!
//! # Example
//!
//! ```
//! use claw_capability::{Capability, Registry};
//! use claw_tool::{Tool, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};
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
//!     .register(Capability::tool(Tool::new(Clock)).with_description("Current time"))
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
mod registry;

pub use capability::{Capability, CapabilityGroup, CapabilityRole};
pub use channel::{ChannelAdapter, OutboundMessage};
pub use error::CapabilityError;
pub use lifecycle::{CapabilityState, Lifecycle};
pub use registry::Registry;
