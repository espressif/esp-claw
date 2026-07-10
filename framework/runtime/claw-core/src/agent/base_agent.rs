//! Layer 2 base agent: a black box that takes **commands** in and reports an
//! outcome out, driven one iteration per [`tick`](BaseAgent::tick).
//!
//! # Command in, outcome out
//!
//! Everything entering the agent is an [`AgentCommand`], queued through the one
//! inbound method [`send_command`](BaseAgent::send_command) — there is no second
//! entry point and no per-command convenience wrapper. Commands queue on an inbox
//! and are reduced by the single funnel `apply_inbound`. Each
//! [`tick`](BaseAgent::tick) returns one [`TickOutcome`]: the agent never has a
//! side-channel of events, because everything it has to report coincides with the
//! moment a tick hands control back.
//!
//! This keeps the agent a uniform unit suitable as the core of a multi-agent
//! system: an orchestrator drives many agents through the identical
//! `send_command` / `tick` triple and never reaches into their internals.
//!
//! # Driving
//!
//! [`tick`](BaseAgent::tick) returns what happened this tick. `Working` means
//! "pump again now"; `Idle` means "nothing to do, wait for a command"; every
//! other variant is a result the driver acts on:
//!
//! ```ignore
//! loop {
//!     match agent.tick() {
//!         TickOutcome::Working => continue,
//!         TickOutcome::Idle => wait_for_command(),
//!         TickOutcome::Yielded { text } => { print(text); wait_for_command(); }
//!         TickOutcome::AwaitingApproval { id, .. } => decide(id),
//!         TickOutcome::Ended { final_message } => { print(final_message); break; }
//!         TickOutcome::Cancelled { .. } => break,
//!         TickOutcome::Failed(error) => { report(error); break; }
//!     }
//! }
//! ```
//!
//! # Termination
//!
//! A plain-text answer is [`Yielded`](TickOutcome::Yielded) — **non-terminal** —
//! and the agent goes idle awaiting the next message. A task ends only when the
//! agent decides so itself (the built-in `end_conversation` tool →
//! [`Ended`](TickOutcome::Ended)), when the orchestrator hard-stops it
//! ([`Cancel`](AgentCommand::Cancel) → [`Cancelled`](TickOutcome::Cancelled)), or
//! on [`Failed`](TickOutcome::Failed). Out-of-band preemption is only an abort
//! signal for the in-flight iteration; task content and task-level control still
//! enter through [`AgentCommand`]. A terminal outcome is reported once and leaves
//! the agent **idle and reusable** — the next
//! [`AppendMessage`](AgentCommand::AppendMessage) starts a fresh task over the
//! same memory and identity once the driver has observed the agent is idle.

mod command;
mod construction;
mod control;
mod iteration;
mod persistence;
mod reducer;
mod state;

use std::sync::Arc;

use claw_api::{ClawApiAsync, RetryPolicy};
use claw_checkpoint::DurableState;
use claw_interface::{ClawHttp, ClawTimer};

use super::iteration_loop::{IterationId, IterationLoopError};
use crate::agent::tools::ControlSink;
use crate::memory::{ContextAdapter, Transcript};
use claw_context::Context;
use claw_permission::PermissionPolicy;
use claw_tool::ToolSet;

pub(crate) use self::command::{
    AgentCommand, AgentCommandError, AgentState, ApprovalDecision, BaseAgentBuildError,
    CancelReason, TickOutcome,
};
pub(crate) use self::construction::BaseAgentConfig;
pub(crate) use self::control::AgentAbortHandle;
use self::control::AgentInterruption;
use self::state::BaseAgentState;

crate::define_prefixed_id!(AgentId, "agent-", "agent");
crate::define_prefixed_id!(ApprovalId, "approval-", "approval");

crate::define_id_allocator!(
    /// This agent's [`IterationId`] counter. Single-owner (a `BaseAgent` field,
    /// advanced through `&mut self`), so it needs no lock; it is reset to a fresh
    /// counter at the start of each task.
    IterationIdAllocator(IterationId),
    IterationId(0)
);
crate::define_id_allocator!(
    /// This agent's [`ApprovalId`] counter. Single-owner (a `BaseAgent` field,
    /// advanced through `&mut self`), so it needs no lock.
    ApprovalIdAllocator(ApprovalId),
    ApprovalId(0)
);

// ===========================================================================
// Internals
// ===========================================================================

// ===========================================================================
// BaseAgent
// ===========================================================================

/// A base agent that runs one task at a time as a sequence of iterations.
///
/// Build once via [`BaseAgent::build`] from a [`BaseAgentConfig`]; then drive it
/// with commands and ticks. The agent is long-lived and reused across tasks — its
/// conversation memory and identity persist, so finishing a task leaves it ready
/// for the next.
///
/// # Examples
///
/// ```ignore
/// let mut agent = BaseAgent::build(BaseAgentConfig {
///     llm_config,
///     store: memory,
///     tools,
///     skills: SkillSet::empty(),
///     agent_instruction: Block::new(BlockKind::AgentInstruction, "You are a helpful assistant."),
///     inherited_context: Arc::from([]),
///     retry_policy: RetryPolicy::default(),
///     permission_policy: Arc::new(claw_permission::AllowAll),
///     block_retries: RetryCount::new(0),
/// })?;
///
/// agent.send_command(AgentCommand::AppendMessage("summarize today's news".into()))?;
/// loop {
///     match agent.tick() {
///         TickOutcome::Working => continue,
///         TickOutcome::Yielded { text } => { println!("{text}"); break; }
///         TickOutcome::Ended { final_message } => { println!("{final_message}"); break; }
///         TickOutcome::Failed(error) => return Err(error.into()),
///         _ => break,
///     }
/// }
/// ```
pub(crate) struct BaseAgent<H: ClawHttp, Timer: ClawTimer> {
    llm: ClawApiAsync<H, Timer>,
    /// Retry policy applied to every per-iteration LLM call.
    retry_policy: RetryPolicy,
    interruption: AgentInterruption,
    /// The conversation transcript's sole owner. The agent **writes** it directly
    /// at each boundary (a user message, a committed answer/tool patch, or an
    /// explicit end marker) and **reads** it to assemble each request
    /// ([`run_iteration`](Self::run_iteration)); it also lends the read view
    /// ([`Transcript::as_history`]) to each [`ContextAdapter`] so they can pull
    /// from it.
    /// Held behind the [`Transcript`] trait object — the agent never sees the
    /// concrete conversation-memory type, which is why it is not generic over a
    /// filesystem. A `Box` (not `Arc`): the agent is the sole owner of this
    /// handle and only calls `&self` methods; the underlying store's own
    /// `Arc<StoreInner>` already provides the sharing the read adapters need.
    transcript: Box<dyn Transcript>,
    /// The agent's tools, including soft-hide phase gating. The registry/tools
    /// are runtime handles; only the tool-state projection is exported.
    tools: ToolSet,
    /// Runtime permission policy; durable human decisions live in `state`.
    permission_policy: Arc<dyn PermissionPolicy>,
    /// The agent's context assembly, owned wholesale by `claw-context`: inherited
    /// blocks, the agent instruction, adapter-projected blocks/reminders, the
    /// cached system prefix, and the ephemeral reminder tail. The agent does not
    /// hand-place adapter sources; they contribute into a context sink, and
    /// [`Context::request`] renders lazily. Change detection, wire ordering, and
    /// reminder rendering all live in the context.
    context: Context,
    /// Durable agent state. Runtime dependencies stay on [`BaseAgent`].
    state: DurableState<BaseAgentState>,
    /// The actionable outcome produced during the current tick, if any. Reset at
    /// the start of each tick; a single tick produces at most one.
    outcome: Option<TickOutcome>,
    /// Sink the built-in tools push [`ControlSignal`]s onto; drained each tick.
    control: ControlSink,
    /// Registered context adapters. They are request-time projectors; any
    /// authoritative external store they read from is durable on its own.
    adapters: Vec<Box<dyn ContextAdapter>>,
}

// ===========================================================================
// Tests: the FSM transition table
// ===========================================================================
