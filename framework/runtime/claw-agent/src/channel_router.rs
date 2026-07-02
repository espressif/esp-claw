use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

use claw_capability::{
    CapabilityError, ChannelAdapter, ChannelFuture, ChannelRuntime, InboundMessage,
    OutboundMessage, Registry,
};
use claw_core::{
    DeliverError, DriveOutput, Orchestrator, SessionId, SessionMessage, SessionRecord,
};

use crate::AgentError;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct ChannelChatKey {
    channel: String,
    chat_id: String,
}

impl ChannelChatKey {
    fn new(channel: &str, chat_id: &str) -> Self {
        Self {
            channel: channel.to_string(),
            chat_id: chat_id.to_string(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct SessionChannelRoute {
    channel: String,
    chat_id: String,
    reply_to_message_id: Option<String>,
}

impl SessionChannelRoute {
    fn new(channel: &str, chat_id: &str) -> Self {
        Self {
            channel: channel.to_string(),
            chat_id: chat_id.to_string(),
            reply_to_message_id: None,
        }
    }

    fn from_inbound(message: &InboundMessage) -> Self {
        Self {
            channel: message.channel.clone(),
            chat_id: message.chat_id.clone(),
            reply_to_message_id: Some(message.message_id.clone()),
        }
    }
}

pub struct ChannelRouter {
    orchestrator: Arc<Orchestrator>,
    channels: HashMap<String, Arc<dyn ChannelAdapter>>,
    chat_sessions: Mutex<HashMap<ChannelChatKey, SessionId>>,
    session_routes: Mutex<HashMap<SessionId, SessionChannelRoute>>,
    open: Mutex<bool>,
}

impl ChannelRouter {
    pub fn new(
        orchestrator: Arc<Orchestrator>,
        registry: &Registry,
    ) -> Result<Arc<Self>, AgentError> {
        let mut channels = HashMap::new();
        for channel in registry.channels() {
            let id = channel.channel_id().to_string();
            if id.trim().is_empty() {
                return Err(AgentError::MissingChannelId);
            }
            if channels.insert(id.clone(), channel).is_some() {
                return Err(AgentError::DuplicateChannel(id));
            }
        }

        Ok(Arc::new(Self {
            orchestrator,
            channels,
            chat_sessions: Mutex::new(HashMap::new()),
            session_routes: Mutex::new(HashMap::new()),
            open: Mutex::new(false),
        }))
    }

    pub fn open(self: &Arc<Self>) -> Result<(), CapabilityError> {
        let runtime: Arc<dyn ChannelRuntime> = Arc::clone(self) as Arc<dyn ChannelRuntime>;
        self.open_with_runtime(runtime)
    }

    pub fn open_with_runtime(
        &self,
        runtime: Arc<dyn ChannelRuntime>,
    ) -> Result<(), CapabilityError> {
        let mut open = self.open.lock().map_err(|_| {
            CapabilityError::Failed("channel router open lock poisoned".to_string())
        })?;
        if *open {
            return Err(CapabilityError::InvalidState);
        }

        let mut opened: Vec<Arc<dyn ChannelAdapter>> = Vec::new();
        for channel in self.channels.values() {
            if let Err(error) = channel.open(Arc::clone(&runtime)) {
                for opened_channel in opened.into_iter().rev() {
                    let _ = opened_channel.close();
                }
                return Err(error);
            }
            opened.push(Arc::clone(channel));
        }

        *open = true;
        Ok(())
    }

    pub fn close(&self) -> Result<(), CapabilityError> {
        let mut open = self.open.lock().map_err(|_| {
            CapabilityError::Failed("channel router open lock poisoned".to_string())
        })?;
        if !*open {
            return Err(CapabilityError::InvalidState);
        }

        let mut first_error = None;
        for channel in self.channels.values() {
            if let Err(error) = channel.close() {
                first_error.get_or_insert(error);
            }
        }
        *open = false;
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    pub async fn push_message(&self, message: InboundMessage) -> Result<(), CapabilityError> {
        self.validate_inbound(&message)?;
        let session = self.session_for(&message)?;
        self.session_routes()
            .insert(session, SessionChannelRoute::from_inbound(&message));

        let output = self
            .orchestrator
            .deliver(
                session,
                SessionMessage::new(message.text, message.message_id, message.sender_id),
            )
            .await
            .map_err(map_deliver_error)?;
        self.surface_output(output)
    }

    pub fn new_session(&self) -> SessionId {
        self.orchestrator.session_create()
    }

    pub fn bind_session(
        &self,
        session: SessionId,
        channel: &str,
        chat_id: &str,
    ) -> Result<(), CapabilityError> {
        let channel = channel.trim();
        let chat_id = chat_id.trim();
        if channel.is_empty() || chat_id.is_empty() {
            return Err(CapabilityError::InvalidArg);
        }
        if !self.channels.contains_key(channel) || !self.orchestrator.session_exists(session) {
            return Err(CapabilityError::NotFound);
        }

        let key = ChannelChatKey::new(channel, chat_id);
        let mut chat_sessions = self.chat_sessions();
        if let Some(existing) = chat_sessions.get(&key) {
            if *existing != session {
                return Err(CapabilityError::AlreadyExists);
            }
        }

        let mut session_routes = self.session_routes();
        if let Some(existing) = session_routes.get(&session) {
            if existing.channel != channel || existing.chat_id != chat_id {
                return Err(CapabilityError::AlreadyExists);
            }
        }

        chat_sessions.insert(key, session);
        session_routes.insert(session, SessionChannelRoute::new(channel, chat_id));
        Ok(())
    }

    pub fn list_sessions(&self) -> Vec<SessionRecord> {
        let routes = self.session_routes();
        self.orchestrator
            .session_list()
            .into_iter()
            .map(|mut record| {
                if let Some(route) = routes.get(&record.id) {
                    record.channel = Some(route.channel.clone());
                    record.chat_id = Some(route.chat_id.clone());
                }
                record
            })
            .collect()
    }

    pub fn delete_session(&self, session: SessionId) -> Result<(), CapabilityError> {
        self.orchestrator
            .session_delete(session)
            .map_err(|error| CapabilityError::Failed(error.to_string()))?;
        self.session_routes().remove(&session);
        self.chat_sessions()
            .retain(|_, routed_session| *routed_session != session);
        Ok(())
    }

    fn validate_inbound(&self, message: &InboundMessage) -> Result<(), CapabilityError> {
        if message.message_id.trim().is_empty()
            || message.channel.trim().is_empty()
            || message.chat_id.trim().is_empty()
            || message.text.trim().is_empty()
        {
            return Err(CapabilityError::InvalidArg);
        }
        if !self.channels.contains_key(&message.channel) {
            return Err(CapabilityError::NotFound);
        }
        Ok(())
    }

    fn session_for(&self, message: &InboundMessage) -> Result<SessionId, CapabilityError> {
        let key = ChannelChatKey::new(&message.channel, &message.chat_id);
        self.chat_sessions()
            .get(&key)
            .copied()
            .ok_or(CapabilityError::NotFound)
    }

    fn surface_output(&self, output: DriveOutput) -> Result<(), CapabilityError> {
        for reply in output.replies {
            self.send_to_session(reply.session, reply.text)?;
        }
        for approval in output.approvals {
            self.send_to_session(
                approval.session,
                format!(
                    "[approval needed: {} {}] {}",
                    approval.agent, approval.approval, approval.summary
                ),
            )?;
        }
        Ok(())
    }

    fn send_to_session(
        &self,
        session: SessionId,
        text: impl Into<String>,
    ) -> Result<(), CapabilityError> {
        let route = self
            .session_routes()
            .get(&session)
            .cloned()
            .ok_or_else(|| CapabilityError::Failed(format!("no reply route for {session}")))?;
        let channel = self
            .channels
            .get(&route.channel)
            .ok_or(CapabilityError::NotFound)?;
        channel.send(&OutboundMessage {
            channel: route.channel,
            chat_id: route.chat_id,
            text: text.into(),
            reply_to_message_id: route.reply_to_message_id,
        })
    }

    fn chat_sessions(&self) -> MutexGuard<'_, HashMap<ChannelChatKey, SessionId>> {
        self.chat_sessions
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn session_routes(&self) -> MutexGuard<'_, HashMap<SessionId, SessionChannelRoute>> {
        self.session_routes
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }
}

impl ChannelRuntime for ChannelRouter {
    fn push_message(&self, message: InboundMessage) -> ChannelFuture<'_> {
        Box::pin(async move { ChannelRouter::push_message(self, message).await })
    }
}

fn map_deliver_error(error: DeliverError) -> CapabilityError {
    match error {
        DeliverError::SessionNotFound(_) => CapabilityError::NotFound,
        DeliverError::Agent(message) => CapabilityError::Failed(message),
        DeliverError::Session(error) => CapabilityError::Failed(error.to_string()),
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use claw_capability::{Capability, Registry};
    use claw_context::Block;
    use claw_core::agent::{Agent, AgentFactory, AgentId, AgentKind, GraphHost};

    use super::*;

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    struct NeverFactory;

    impl AgentFactory for NeverFactory {
        fn create_agent(
            &self,
            _id: AgentId,
            _kind: &AgentKind,
            _goal: String,
            _host: Arc<dyn GraphHost>,
            _is_root: bool,
            _inherited_context: Arc<[Block<'static>]>,
        ) -> Result<Box<dyn Agent>, String> {
            Err("unbound inbound must not create an agent".to_string())
        }
    }

    struct TestChannel;

    impl ChannelAdapter for TestChannel {
        fn channel_id(&self) -> &str {
            "web"
        }

        fn open(&self, _runtime: Arc<dyn ChannelRuntime>) -> Result<(), CapabilityError> {
            Ok(())
        }

        fn close(&self) -> Result<(), CapabilityError> {
            Ok(())
        }

        fn send(&self, _message: &OutboundMessage) -> Result<(), CapabilityError> {
            Ok(())
        }
    }

    #[test]
    fn unbound_inbound_does_not_create_session() {
        let registry = Registry::new();
        registry
            .register(Capability::channel(Arc::new(TestChannel)))
            .unwrap();
        let orchestrator = Orchestrator::new(Arc::new(NeverFactory));
        let router = ChannelRouter::new(orchestrator, &registry).unwrap();
        let existing = router.new_session();

        let error = block_on(router.push_message(InboundMessage {
            message_id: "m1".into(),
            channel: "web".into(),
            chat_id: "chat".into(),
            sender_id: None,
            text: "hello".into(),
        }))
        .unwrap_err();

        assert_eq!(error, CapabilityError::NotFound);
        let sessions = router.list_sessions();
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions.first().unwrap().id, existing);
    }
}
