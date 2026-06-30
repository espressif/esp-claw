//! In-memory channel implementations for host tests and default wiring.

use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex, RwLock};

use super::egress::{ChannelEgress, ChannelTransport};
use super::ingress::{ChannelIngress, ChannelIngressSink};
use super::message::{ChannelError, InboundCommand, InboundMessage, OutboundMessage};

/// In-memory ingress: separate user-message and command queues.
pub struct LocalChannelIngress {
    user_messages: Mutex<VecDeque<InboundMessage>>,
    commands: Mutex<VecDeque<InboundCommand>>,
}

impl Default for LocalChannelIngress {
    fn default() -> Self {
        Self::new()
    }
}

impl LocalChannelIngress {
    pub fn new() -> Self {
        Self {
            user_messages: Mutex::new(VecDeque::new()),
            commands: Mutex::new(VecDeque::new()),
        }
    }
}

impl ChannelIngressSink for LocalChannelIngress {
    fn push_user_message(&self, msg: InboundMessage) {
        self.user_messages.lock().unwrap().push_back(msg);
    }

    fn push_command(&self, command: InboundCommand) {
        self.commands.lock().unwrap().push_back(command);
    }
}

impl ChannelIngress for LocalChannelIngress {
    fn drain_user_messages(&mut self) -> Vec<InboundMessage> {
        self.user_messages.lock().unwrap().drain(..).collect()
    }

    fn drain_commands(&mut self) -> Vec<InboundCommand> {
        self.commands.lock().unwrap().drain(..).collect()
    }
}

/// Routes outbound messages to registered [`ChannelTransport`] adapters.
pub struct ChannelEgressHub {
    transports: RwLock<HashMap<String, Arc<dyn ChannelTransport>>>,
    unrouted: Mutex<Vec<OutboundMessage>>,
}

impl Default for ChannelEgressHub {
    fn default() -> Self {
        Self::new()
    }
}

impl ChannelEgressHub {
    pub fn new() -> Self {
        Self {
            transports: RwLock::new(HashMap::new()),
            unrouted: Mutex::new(Vec::new()),
        }
    }

    pub fn register(&self, transport: Arc<dyn ChannelTransport>) {
        let id = transport.id().to_string();
        if let Ok(mut transports) = self.transports.write() {
            transports.insert(id, transport);
        }
    }

    pub fn drain_unrouted(&self) -> Vec<OutboundMessage> {
        self.unrouted.lock().unwrap().drain(..).collect()
    }
}

impl ChannelEgress for ChannelEgressHub {
    fn send(&self, msg: &OutboundMessage) -> Result<(), ChannelError> {
        let transports = self
            .transports
            .read()
            .map_err(|_| ChannelError::SendFailed("transport registry poisoned".into()))?;
        if let Some(transport) = transports.get(&msg.channel) {
            transport.send(msg)
        } else {
            self.unrouted.lock().unwrap().push(msg.clone());
            Ok(())
        }
    }
}

/// Records outbound messages for assertions (host tests).
pub struct RecordingTransport {
    id: String,
    sent: Mutex<Vec<OutboundMessage>>,
}

impl RecordingTransport {
    pub fn new(id: impl Into<String>) -> Arc<Self> {
        Arc::new(Self {
            id: id.into(),
            sent: Mutex::new(Vec::new()),
        })
    }

    pub fn drain_sent(&self) -> Vec<OutboundMessage> {
        self.sent.lock().unwrap().drain(..).collect()
    }
}

impl ChannelTransport for RecordingTransport {
    fn id(&self) -> &str {
        &self.id
    }

    fn send(&self, msg: &OutboundMessage) -> Result<(), ChannelError> {
        self.sent.lock().unwrap().push(msg.clone());
        Ok(())
    }
}
