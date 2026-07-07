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
pub use claw_utils::IdParseError;
pub use event::AgentEvent;
pub use orchestrator::{
    DriveOutput, Orchestrator, OrchestratorBuildError, RootReply, SubmitStream,
};
pub use session::{DeliverError, DeliveryKind, SessionError, SessionId};
