//! Agents driven by the orchestrator.
//!
//! [`BaseAgent`] is the command-in / outcome-out base layer. There is exactly one
//! semantic agent on top of it — [`GenericAgent`] — a flat ReAct loop with no
//! built-in FSM. What distinguishes one agent "kind" from another is pure data:
//! its [`AgentConfig`] (system prompt, tool set, skills, spawning), loaded at
//! compile time from `resources/agents/<kind>/` and assembled by the factory.
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
mod tools;

pub(crate) use base_agent::{
    AgentAbortHandle, AgentCommand, AgentCommandError, AgentId, ApprovalDecision, ApprovalId,
    CancelReason, TickOutcome,
};
pub(crate) use factory::{AgentPlacement, FsAgentCreateError, FsAgentFactory, FsAgentFactoryError};
pub(crate) use graph::{AgentSnapshot, AgentStatus, GraphEffect, GraphHost, TerminationPolicy};
pub use iteration_loop::IterationId;
pub(crate) use iteration_loop::{
    ChatMessages, CompletedKind, InterruptionControl, IterationLoop, IterationLoopError,
    IterationOutcome, IterationStep, SystemPrompt,
};
pub(crate) use kind::AgentKind;
pub(crate) use registry::{AgentIdAllocator, AgentRegistry};

use crate::event::EventSink;

use claw_checkpoint::{DurablePart, DurablePartError, PartStateSlice};
use core::future::Future;
use core::pin::Pin;

pub(crate) type AgentTickFuture<'a> = Pin<Box<dyn Future<Output = TickOutcome> + 'a>>;

/// The unified contract a scheduler drives any agent through.
///
/// Object-safe so heterogeneous agents can be held as `Box<dyn Agent>`. The
/// surface mirrors [`BaseAgent`]: commands go in via [`send_command`](Agent::send_command),
/// outcomes come out via [`tick`](Agent::tick). Multi-agent graph inputs use
/// separate child ports rather than [`AgentCommand`] variants, so the external
/// command vocabulary stays untouched.
pub(crate) trait Agent: DurablePart {
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

    /// Durable subparts owned by this agent.
    ///
    /// The agent trait stays crate-private, so this is an orchestrator/checkpoint
    /// contract rather than public object introspection.
    fn durable_parts(&self) -> Vec<&dyn DurablePart> {
        Vec::new()
    }

    /// Restore one durable subpart by name. Returns `Ok(false)` when the name is
    /// not owned by this agent implementation.
    fn restore_durable_part(
        &mut self,
        name: &str,
        state: PartStateSlice<'_>,
    ) -> Result<bool, DurablePartError> {
        let _ = (name, state);
        Ok(false)
    }

    /// Advance the agent by one step and report what happened. See [`TickOutcome`].
    ///
    /// `events` is the per-turn [`EventSink`] this agent's iteration should emit to.
    /// The instance hands the **root** agent the submission's live sink and every
    /// subagent a [disabled](EventSink::disabled) one, so only the root's
    /// iteration events reach the stream.
    fn tick(&mut self, events: EventSink) -> AgentTickFuture<'_>;
}
