//! Channel ingress/egress: messages, traits, and default in-memory implementations.

mod egress;
mod ingress;
mod local;
mod message;

pub use egress::{ChannelEgress, ChannelTransport};
pub use ingress::{ChannelIngress, ChannelIngressSink, IngressFuture};
pub use local::{ChannelEgressHub, LocalChannelIngress, RecordingTransport};
pub use message::{
    ChannelError, Command, InboundCommand, InboundMessage, OutboundMessage, ReplyRoute,
};
