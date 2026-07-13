//! Agent manifests, construction, task execution, and graph tools.
//!
//! Agent kinds are data; [`BaseAgent`] is the sole runtime implementation.

mod base_agent;
mod config;
mod factory;
mod graph;
mod iteration_loop;
mod kind;
mod manifest;
mod registry;
mod tools;

pub(crate) use base_agent::{
    AgentAbortHandle, AgentCommand, AgentCommandError, AgentId, ApprovalDecision, BaseAgent,
    TickOutcome,
};
pub(crate) use factory::{AgentPlacement, FsAgentCreateError, FsAgentFactory, FsAgentFactoryError};
pub(crate) use graph::{
    is_strict_descendant, AgentGraphSnapshot, AgentSnapshot, AgentStatus, GraphEffect, GraphHost,
    TerminationPolicy,
};
pub use iteration_loop::IterationId;
pub(crate) use iteration_loop::{
    CompletedKind, InterruptionControl, IterationLoop, IterationLoopError, IterationOutcome,
    IterationStep,
};
pub(crate) use kind::AgentKind;
pub(crate) use registry::{AgentIdAllocator, AgentRegistry};
