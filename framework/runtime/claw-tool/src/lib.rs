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
//!
//! Sync and async execution are both intentional public surfaces. The sync path
//! is not migration leftover: immediate and C-backed tools can remain sync, while
//! Rust tools that await cooperative work use [`AsyncToolHandler`]. The agent
//! runtime drives tools through [`ToolRunner::run_one_async`], and sync handlers
//! remain valid there because [`Tool::invoke_async`] moves their body onto the
//! fixed tool executor instead of blocking the main agent executor.

pub mod bake;

mod block;
mod executor;
mod gate;
mod handler;
mod registry;
mod runner;
mod set;
mod validate;

pub use block::{BlockPolicy, ToolBlockVerdict};
pub use executor::init_tool_executor;
pub use gate::PermissionGate;
pub use handler::{
    AsyncToolHandler, Tool, ToolError, ToolFuture, ToolHandler, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolRetryCount,
};
pub use registry::{ToolRegistry, ToolRegistryError};
pub use runner::{ApprovalNeeded, CallOutcome, ToolGate, ToolRunner};
pub use set::{AllowedTools, ToolGroup, ToolSet, ToolSetError};
