//! Agent-graph types and the façade internal tools use to reach the orchestrator.

mod context;
mod spawn_policy;
mod types;

pub(crate) use context::{AgentContext, GraphHost};
pub(crate) use spawn_policy::SpawnPolicy;
pub(crate) use types::{AgentSnapshot, AgentStatus, GraphEffect, TerminationPolicy};
