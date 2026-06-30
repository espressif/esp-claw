//! The message-channel role: a capability that ingests inbound messages and/or
//! sends outbound ones (IM platforms, the local/web channel).
//!
//! A channel capability unifies the inbound gateway and the outbound send verbs:
//!
//! - **Outbound** is the [`ChannelAdapter::send`] egress below — *not* an
//!   LLM-callable tool.
//! - **Inbound** is driven by the channel's [`Lifecycle`](crate::Lifecycle)
//!   (the transport task started in `start`), which pushes messages to a sink
//!   the host injects at construction. The sink type is intentionally *not*
//!   defined here yet — wiring inbound to the orchestrator's ingress is a later
//!   step.

use crate::error::CapabilityError;

/// An agent reply routed back out through a channel transport.
///
/// Deliberately minimal — `{channel, chat_id, text, reply_to}` is everything an
/// outbound send needs. When the channel half is wired end-to-end this will be
/// reconciled with `claw_core::channels::OutboundMessage`; kept local for now to
/// avoid an upward crate dependency.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OutboundMessage {
    pub channel: String,
    pub chat_id: String,
    pub text: String,
    pub reply_to_message_id: Option<String>,
}

/// The outbound side of a message channel: deliver one reply to a transport.
///
/// A `ChannelAdapter` almost always also owns a [`Lifecycle`](crate::Lifecycle)
/// (its transport task); register the same concrete object into both the
/// [`Channel`](crate::CapabilityRole::Channel) role and the
/// [`lifecycle`](crate::Capability::lifecycle) slot.
pub trait ChannelAdapter: Send + Sync {
    /// The channel id this adapter serves (e.g. `"telegram"`, `"local"`).
    fn channel_id(&self) -> &str;

    /// Deliver `message` to the underlying transport.
    fn send(&self, message: &OutboundMessage) -> Result<(), CapabilityError>;
}
