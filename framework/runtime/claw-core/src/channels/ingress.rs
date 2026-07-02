//! Inbound channel port: user messages and orchestrator commands.
//!
//! External drivers (IM, CLI, event router adapters) push via
//! [`ChannelIngressSink`]. [`crate::Orchestrator`] implements
//! it and delivers into orchestrator callbacks on push.

use super::message::{InboundCommand, InboundMessage};
use crate::session::DeliverError;
use core::future::Future;
use core::pin::Pin;

pub type IngressFuture<'a> = Pin<Box<dyn Future<Output = Result<(), DeliverError>> + 'a>>;

/// Producer side: adapters push inbound work here.
pub trait ChannelIngressSink: Send + Sync {
    fn push_user_message(&self, msg: InboundMessage) -> IngressFuture<'_>;
    fn push_command(&self, command: InboundCommand) -> IngressFuture<'_>;
}

/// Consumer side: orchestrator drains user text and commands (separate queues, not an enum).
pub trait ChannelIngress: ChannelIngressSink {
    fn drain_user_messages(&mut self) -> Vec<InboundMessage>;
    fn drain_commands(&mut self) -> Vec<InboundCommand>;
}
