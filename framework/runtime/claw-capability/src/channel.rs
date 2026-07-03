//! The bidirectional message-channel role.
//!
//! A channel capability is opened with a [`ChannelRuntime`] and can then push
//! inbound user messages through that runtime. Agent replies flow back out
//! through [`ChannelAdapter::send`].

use core::future::Future;
use core::pin::Pin;
use std::sync::Arc;

use crate::error::CapabilityError;

pub type ChannelFuture<'a> = Pin<Box<dyn Future<Output = Result<(), CapabilityError>> + 'a>>;

/// User or IM message submitted by a channel.
///
/// `session_id` is intentionally absent: the agent runtime accepts this message
/// only after `(channel, chat_id)` has been explicitly bound to a session.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct InboundMessage {
    pub message_id: String,
    pub channel: String,
    pub chat_id: String,
    pub sender_id: Option<String>,
    pub text: String,
    /// Opaque, transport-neutral extra context for this message. Carried through
    /// to the orchestrator boundary, where the driving layer interprets it (e.g.
    /// resolving a delivery mode such as interrupt/cancel). Kept a bare string so
    /// the transport layer stays free of any orchestration/scheduling vocabulary;
    /// `None` when the transport has nothing extra to say.
    pub extra_context: Option<String>,
}

/// An agent reply routed back out through a channel transport.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OutboundMessage {
    pub channel: String,
    pub chat_id: String,
    pub text: String,
    pub reply_to_message_id: Option<String>,
}

/// Runtime injected into a channel when the agent system starts.
pub trait ChannelRuntime {
    /// Submit one inbound message into the agent runtime.
    fn push_message(&self, message: InboundMessage) -> ChannelFuture<'_>;
}

/// Bidirectional channel adapter.
pub trait ChannelAdapter: Send + Sync {
    /// The channel id this adapter serves (e.g. `"telegram"`, `"local"`).
    fn channel_id(&self) -> &str;

    /// Bind the channel to the agent runtime and start its receive side.
    fn open(&self, runtime: Arc<dyn ChannelRuntime>) -> Result<(), CapabilityError>;

    /// Stop the receive side and release the injected runtime.
    fn close(&self) -> Result<(), CapabilityError>;

    /// Deliver `message` to the underlying transport.
    fn send(&self, message: &OutboundMessage) -> Result<(), CapabilityError>;
}
