//! Channel message types and routing metadata.

use crate::protocol::Command;
use crate::session::SessionId;

use thiserror::Error;

/// Reply routing snapshot for one session (updated on each inbound).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReplyRoute {
    pub channel: String,
    pub chat_id: String,
    pub sender_id: Option<String>,
    pub reply_to_message_id: Option<String>,
}

impl ReplyRoute {
    pub fn from_inbound(msg: &InboundMessage) -> Self {
        Self {
            channel: msg.channel.clone(),
            chat_id: msg.chat_id.clone(),
            sender_id: msg.sender_id.clone(),
            reply_to_message_id: Some(msg.message_id.clone()),
        }
    }
}

/// Orchestrator command scoped to one session (framework routes by [`SessionId`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InboundCommand {
    pub session_id: SessionId,
    pub command: Command,
}

/// User or IM message submitted from an external channel adapter.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InboundMessage {
    pub message_id: String,
    pub channel: String,
    pub chat_id: String,
    pub sender_id: Option<String>,
    pub session_id: String,
    pub text: String,
}

/// Agent reply routed back through a channel adapter.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OutboundMessage {
    pub channel: String,
    pub chat_id: String,
    pub text: String,
    pub reply_to_message_id: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum ChannelError {
    #[error("channel not found: {0}")]
    ChannelNotFound(String),
    #[error("no reply route for session: {0}")]
    SessionRouteNotFound(String),
    #[error("channel send failed: {0}")]
    SendFailed(String),
}
