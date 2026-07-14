//! Runtime for one agent in isolation.
//!
//! This module owns the agent state machine, LLM/tool loop, context adapters,
//! and construction. Graphs, child lifecycles, and subagent tools belong to the
//! orchestrator and enter here only as ordinary injected tool groups.

mod base_agent;
mod config;
mod factory;
mod iteration_loop;
mod tools;

pub(crate) use base_agent::{
    AgentAbortHandle, AgentCommand, AgentCommandError, ApprovalDecision, BaseAgent, TickOutcome,
};
pub(crate) use factory::{
    AgentEnvironment, FsAgentCreateError, FsAgentFactory, FsAgentFactoryError, ProfileAccess,
    TranscriptTarget,
};
pub(crate) use iteration_loop::{
    CompletedKind, InterruptionControl, IterationLoop, IterationLoopError, IterationOutcome,
    IterationStep,
};
