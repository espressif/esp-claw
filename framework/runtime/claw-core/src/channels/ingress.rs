//! Inbound channel port: user messages and orchestrator commands.
//!
//! External drivers (IM, CLI, event router adapters) push via
//! [`ChannelIngressSink`]. [`crate::Orchestrator`] implements
//! it and delivers into orchestrator callbacks on push.

use super::message::{InboundCommand, InboundMessage};

/// Producer side: adapters push inbound work here.
pub trait ChannelIngressSink: Send + Sync {
    fn push_user_message(&self, msg: InboundMessage);
    fn push_command(&self, command: InboundCommand);
}

/// Consumer side: orchestrator drains user text and commands (separate queues, not an enum).
pub trait ChannelIngress: ChannelIngressSink {
    fn drain_user_messages(&mut self) -> Vec<InboundMessage>;
    fn drain_commands(&mut self) -> Vec<InboundCommand>;
}
