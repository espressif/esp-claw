//! Agents driven by the orchestrator.
//!
//! [`BaseAgent`] is the command-in / outcome-out base layer. There is exactly one
//! semantic agent on top of it — [`GenericAgent`] — a flat ReAct loop with no
//! built-in FSM. What distinguishes one agent "kind" from another is pure data:
//! its [`AgentConfig`] (system prompt, tool set, skills, spawning), loaded at
//! compile time from `resources/agents/<kind>/` and resolved through an
//! [`AgentResolver`] — see the crate-internal `manifest` module.
//! [`GenericAgent`] implements
//! the unified [`Agent`] trait the scheduler drives; the trait never reaches into
//! internals.
//!
//! Spawning is a model-callable `spawn_subagent(kind, goal)` tool in the
//! crate-internal tool module. It emits a [`GraphEffect`] through a [`GraphHost`];
//! the crate-internal orchestrator instance owns the flattened agent graph and
//! materializes children through [`FsAgentFactory`].

mod base_agent;
mod config;
mod factory;
mod generic_agent;
mod graph;
mod iteration_loop;
mod kind;
mod manifest;
mod registry;
mod resolver;
mod tools;

pub use base_agent::{
    AgentAbortHandle, AgentCommand, AgentCommandError, AgentId, ApprovalDecision, ApprovalId,
    CancelReason, TickOutcome,
};
pub use config::{AgentConfig, AgentConfigError};
pub use factory::{AgentPlacement, FsAgentFactory, FsAgentFactoryError};
pub use generic_agent::{GenericAgent, GenericAgentBuildError};
pub use graph::{
    AgentSnapshot, AgentStatus, ApprovalVerdict, GraphEffect, GraphHost, TerminationPolicy,
};
pub use iteration_loop::IterationId;
// Re-exported only so the orchestrator instance's tests (outside the `agent`
// module) can build agents over an `AgentContext`; the runtime uses it via the
// in-module path.
#[cfg(test)]
pub(crate) use graph::AgentContext;
pub use kind::AgentKind;
pub(crate) use registry::{AgentIdAllocator, AgentRegistry};
pub use resolver::{AgentResolver, MapAgentResolver};

#[doc(no_inline)]
pub use claw_api::RetryPolicy;

use core::future::Future;
use core::pin::Pin;

pub type AgentTickFuture<'a> = Pin<Box<dyn Future<Output = TickOutcome> + 'a>>;

/// The unified contract a scheduler drives any agent through.
///
/// Object-safe so heterogeneous agents can be held as `Box<dyn Agent>`. The
/// surface mirrors [`BaseAgent`]: commands go in via [`send_command`](Agent::send_command),
/// outcomes come out via [`tick`](Agent::tick). The one multi-agent extension is
/// [`deliver_child_result`](Agent::deliver_child_result) — the channel a parent
/// receives a finished subagent's result on (a separate port rather than a new
/// [`AgentCommand`] variant, so the base command vocabulary stays untouched).
pub trait Agent {
    /// This agent's stable identity.
    fn id(&self) -> AgentId;

    /// Hand the agent one command. See [`AgentCommand`] for the vocabulary and
    /// [`AgentCommandError`] for when a command is illegal in the current state.
    fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError>;

    /// Deliver a finished subagent's result back to this (parent) agent.
    ///
    /// The result re-enters as ordinary information for the model to reason over
    /// (it does not preempt or gate anything); the agent owns how it is presented.
    fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool);

    /// A cloneable handle to abort this agent's in-flight LLM/tool round from
    /// another task.
    ///
    /// The handle shares the `Arc<AtomicBool>` the iteration loop polls at its
    /// checkpoints, so it can stop a `tick` blocked on the LLM HTTP call. Grab it
    /// **before** driving (you cannot borrow the agent while a `tick` holds
    /// `&mut self`); it stays valid even while the agent is moved into a tick
    /// future, because it is just an `Arc` clone of the flag — not a borrow of the
    /// agent. It is plumbing for stopping a now-stale call; the *content* of any
    /// new input still arrives as an [`AgentCommand`].
    fn abort_handle(&self) -> AgentAbortHandle;

    /// Advance the agent by one step and report what happened. See [`TickOutcome`].
    fn tick(&mut self) -> AgentTickFuture<'_>;
}
