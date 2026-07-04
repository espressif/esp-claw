//! `claw_core` — runtime primitives for the agent orchestrator.
//!
//! Layer 1: [`Orchestrator`]

mod agent;
mod memory;
mod orchestrator;
mod session;

pub(crate) use claw_utils::{define_id_allocator, define_prefixed_id};

pub use claw_utils::IdParseError;
pub use orchestrator::{DriveOutput, Orchestrator, OrchestratorBuildError, RootReply};
pub use session::{DeliverError, DeliveryKind, SessionError, SessionId};
