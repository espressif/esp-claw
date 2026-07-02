//! `claw-tool` — the tool framework: define a model-callable tool once, bundle
//! tools into a set, gate and execute calls, and enforce the on-disk tool
//! contract at build time.
//!
//! The crate sits below `claw-core` (and beside `claw-permission`): it knows
//! nothing about agents or the orchestrator, only about *tools*. Four layers:
//!
//! - **define** ([`handler`]): the sync [`ToolHandler`] and Rust-side
//!   [`AsyncToolHandler`] traits, plus the [`tool_metadata!`] macro that bakes a
//!   tool's `name`/`schema`/`usage` from `resources/tools/<name>/`, and the
//!   cheap-to-clone [`Tool`] value.
//! - **aggregate** ([`set`]): [`ToolGroup`] and the per-agent [`ToolSet`] —
//!   combined schema JSON + flat dispatch, plus the **soft-tools** state it fully
//!   owns: the [`AllowedTools`] phase allow-set ([`set_active_tools`](ToolSet::set_active_tools)
//!   / [`is_allowed`](ToolSet::is_allowed)) and its two prompt surfaces, the static
//!   [`tool_context`](ToolSet::tool_context) and the dynamic
//!   [`extra_tool_context`](ToolSet::extra_tool_context) phase note.
//! - **registry** ([`registry`]): the [`ToolRegistry`] pool that both baked and
//!   runtime-registered tools live in; [`ToolSet`]s are assembled from it.
//! - **execute** ([`runner`]): the [`ToolRunner`] boundary — soft-hide gating, the
//!   permission [`ToolGate`], and dispatch — shaped for future async concurrency.
//!   [`PermissionGate`] (in [`gate`]) is the policy-backed `ToolGate` the agent
//!   installs.
//! - **block policy** ([`block`]): [`BlockPolicy`], the soft-hide "retry then
//!   fail" streak counter. It is *conversation state* the agent owns, kept out of
//!   [`ToolSet`] (which holds only the immutable catalog and cached wire bytes).
//!
//! The on-disk contract those layers rely on (`resources/tools/<name>/` holds
//! exactly `schema.json` + `usage.md`, and the directory name equals the schema's
//! `function.name`) is enforced at build time by [`bake`] — the *same* crate
//! that defines the macro reading those files, so the runtime and build-time
//! halves of the contract can never drift.

pub mod bake;

mod block;
mod executor;
mod gate;
mod handler;
mod registry;
mod runner;
mod set;
mod validate;

pub use block::{BlockPolicy, ToolBlockVerdict, DEFAULT_BLOCK_RETRIES};
pub use gate::PermissionGate;
pub use handler::{
    tool_invoke_err, tool_invoke_err_with_retries, AsyncToolHandler, Tool, ToolError, ToolFuture,
    ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolRetryCount,
};
pub use registry::ToolRegistry;
pub use runner::{ApprovalNeeded, CallOutcome, ToolGate, ToolRunner};
pub use set::{AllowedTools, ToolGroup, ToolSet, ToolSetError, DEFAULT_TOOL_GROUP};
