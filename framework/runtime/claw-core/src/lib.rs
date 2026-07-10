//! `claw_core` — runtime primitives for the agent orchestrator.
//!
//! Layer 1: [`Orchestrator`]

mod agent;
mod event;
mod memory;
mod orchestrator;
mod session;

pub(crate) use claw_utils::{define_id_allocator, define_prefixed_id};

pub use agent::IterationId;
pub use event::{SessionEvent, TurnCause};
pub use orchestrator::{
    OpenSessionError, Orchestrator, OrchestratorBuildError, SessionControl, SessionControlError,
    SessionEventStream,
};
pub use session::{AttachmentId, AttachmentKind, AttachmentRecord, AttachmentRef, Message};
pub use session::{SessionId, TurnId};
