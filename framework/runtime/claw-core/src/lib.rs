#![deny(unreachable_pub)]

//! `claw_core` — runtime primitives for the agent orchestrator.
//!
//! Layer 1: [`Orchestrator`]

mod agent;
mod config;
mod memory;
mod multiagent;
mod orchestrator;
mod protocol;
mod session;

pub(crate) use claw_utils::{define_id_allocator, define_prefixed_id};

pub use claw_permission::PermissionLevel;
pub use config::{ApiUsage, ReasoningEffort};
pub use orchestrator::{Orchestrator, OrchestratorBuildError};
pub use protocol::{
    AgentId, InputRequestId, InputRequestKind, IterationId, Message, SessionEvent, SessionId,
    SessionPersistence, StreamPart, ToolCall, TurnId, TurnOrigin,
};
pub use session::{OpenSessionError, SessionControl, SessionControlError, SessionEventStream};
