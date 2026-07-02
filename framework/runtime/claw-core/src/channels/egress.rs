//! Outbound channel port and per-transport adapters.

use super::message::{ChannelError, OutboundMessage};

/// Outbound port: orchestrator sends agent replies through this trait.
pub trait ChannelEgress {
    fn send(&self, msg: &OutboundMessage) -> Result<(), ChannelError>;
}

/// One registered IM / CLI transport behind [`super::local::ChannelEgressHub`].
pub trait ChannelTransport {
    fn id(&self) -> &str;
    fn send(&self, msg: &OutboundMessage) -> Result<(), ChannelError>;
}
