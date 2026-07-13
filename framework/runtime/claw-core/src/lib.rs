//! `claw_core` — runtime primitives for the agent orchestrator.
//!
//! Layer 1: [`Orchestrator`]

mod agent;
mod config;
mod event;
mod memory;
mod orchestrator;
mod session;

pub(crate) use claw_utils::{define_id_allocator, define_prefixed_id};

pub use agent::IterationId;
pub use config::{ApiUsage, ClawApiManager};
pub use event::SessionEvent;
pub use orchestrator::{
    OpenSessionError, Orchestrator, OrchestratorBuildError, ReasoningEffort, SessionControl,
    SessionControlError, SessionEventStream,
};
pub use session::{AttachmentId, AttachmentKind, AttachmentRecord, AttachmentRef, Message};
pub use session::{SessionId, SessionPersistence, TurnId};
