//! Immutable values shared across the runtime layers.
//!
//! This module owns wire/domain values only. Runtime state, channels, stores,
//! factories, and schedulers belong to their respective higher-level modules.

mod event;
mod ids;
mod kind;
mod message;

pub(crate) use event::EventSink;
pub use event::{SessionEvent, StreamPart, ToolCall};
pub use ids::{AgentId, IterationId, SessionId, TurnId};
pub(crate) use ids::{IterationIdAllocator, SessionIdAllocator, TurnIdAllocator};
pub(crate) use kind::AgentKind;
pub(crate) use message::deserialize_message_or_text;
pub use message::Message;

use serde::{Deserialize, Serialize};

/// What caused a root-visible turn to start.
#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum TurnOrigin {
    /// A public caller submitted a message.
    #[default]
    User,
    /// A detached subagent delivered its result to its parent.
    Subagent {
        /// The subagent whose result caused this turn.
        agent: AgentId,
    },
}

impl TurnOrigin {
    pub(crate) fn is_user(&self) -> bool {
        matches!(self, Self::User)
    }
}

/// Whether a session survives a runtime restart.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SessionPersistence {
    /// Checkpoint session state and write the root transcript to storage.
    Persistent,
    /// Keep session state and transcript in memory for this process only.
    Ephemeral,
}
