//! In-memory channel implementations for host tests and default wiring.

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::sync::Arc;

use super::egress::{ChannelEgress, ChannelTransport};
use super::ingress::{ChannelIngress, ChannelIngressSink, IngressFuture};
use super::message::{ChannelError, InboundCommand, InboundMessage, OutboundMessage};

/// In-memory ingress: separate user-message and command queues.
pub struct LocalChannelIngress {
    user_messages: RefCell<VecDeque<InboundMessage>>,
    commands: RefCell<VecDeque<InboundCommand>>,
}

impl Default for LocalChannelIngress {
    fn default() -> Self {
        Self::new()
    }
}

impl LocalChannelIngress {
    pub fn new() -> Self {
        Self {
            user_messages: RefCell::new(VecDeque::new()),
            commands: RefCell::new(VecDeque::new()),
        }
    }
}

impl ChannelIngressSink for LocalChannelIngress {
    fn push_user_message(&self, msg: InboundMessage) -> IngressFuture<'_> {
        Box::pin(async move {
            self.user_messages.borrow_mut().push_back(msg);
            Ok(())
        })
    }

    fn push_command(&self, command: InboundCommand) -> IngressFuture<'_> {
        Box::pin(async move {
            self.commands.borrow_mut().push_back(command);
            Ok(())
        })
    }
}

impl ChannelIngress for LocalChannelIngress {
    fn drain_user_messages(&mut self) -> Vec<InboundMessage> {
        self.user_messages.borrow_mut().drain(..).collect()
    }

    fn drain_commands(&mut self) -> Vec<InboundCommand> {
        self.commands.borrow_mut().drain(..).collect()
    }
}

/// Routes outbound messages to registered [`ChannelTransport`] adapters.
pub struct ChannelEgressHub {
    transports: RefCell<HashMap<String, Arc<dyn ChannelTransport>>>,
    unrouted: RefCell<Vec<OutboundMessage>>,
}

impl Default for ChannelEgressHub {
    fn default() -> Self {
        Self::new()
    }
}

impl ChannelEgressHub {
    pub fn new() -> Self {
        Self {
            transports: RefCell::new(HashMap::new()),
            unrouted: RefCell::new(Vec::new()),
        }
    }

    pub fn register(&self, transport: Arc<dyn ChannelTransport>) {
        let id = transport.id().to_string();
        self.transports.borrow_mut().insert(id, transport);
    }

    pub fn drain_unrouted(&self) -> Vec<OutboundMessage> {
        self.unrouted.borrow_mut().drain(..).collect()
    }
}

impl ChannelEgress for ChannelEgressHub {
    fn send(&self, msg: &OutboundMessage) -> Result<(), ChannelError> {
        let transport = self.transports.borrow().get(&msg.channel).cloned();
        if let Some(transport) = transport {
            transport.send(msg)
        } else {
            self.unrouted.borrow_mut().push(msg.clone());
            Ok(())
        }
    }
}

/// Records outbound messages for assertions (host tests).
pub struct RecordingTransport {
    id: String,
    sent: RefCell<Vec<OutboundMessage>>,
}

impl RecordingTransport {
    pub fn new(id: impl Into<String>) -> Arc<Self> {
        Arc::new(Self {
            id: id.into(),
            sent: RefCell::new(Vec::new()),
        })
    }

    pub fn drain_sent(&self) -> Vec<OutboundMessage> {
        self.sent.borrow_mut().drain(..).collect()
    }
}

impl ChannelTransport for RecordingTransport {
    fn id(&self) -> &str {
        &self.id
    }

    fn send(&self, msg: &OutboundMessage) -> Result<(), ChannelError> {
        self.sent.borrow_mut().push(msg.clone());
        Ok(())
    }
}
