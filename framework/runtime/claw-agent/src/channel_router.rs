use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

use claw_capability::{
    CapabilityError, ChannelAdapter, ChannelFuture, ChannelRuntime, InboundMessage,
    OutboundMessage, Registry,
};
use claw_core::{
    DeliverError, DeliveryKind, DriveOutput, DriveStop, Orchestrator, SessionBinding,
    SessionControl, SessionId, SessionMessage, SessionRecord,
};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

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

pub struct ChannelRouter<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    orchestrator: Arc<Orchestrator<F, H, Timer>>,
    channels: HashMap<String, Arc<dyn ChannelAdapter>>,
    chat_sessions: Mutex<HashMap<ChannelChatKey, SessionId>>,
    session_routes: Mutex<HashMap<SessionId, SessionChannelRoute>>,
    open: Mutex<bool>,
}

impl<F, H, Timer> ChannelRouter<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub fn new(
        orchestrator: Arc<Orchestrator<F, H, Timer>>,
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

        // Rebuild the in-memory routing tables from whatever session bindings the
        // orchestrator currently exposes. Only bindings for channels this router
        // actually has are restored; a binding whose channel is gone is dropped.
        // The per-message `reply_to_message_id` is ephemeral — the next inbound
        // message repopulates it.
        let mut chat_sessions = HashMap::new();
        let mut session_routes = HashMap::new();
        for record in orchestrator.session_list() {
            if let Some(binding) = &record.binding {
                if channels.contains_key(&binding.channel) {
                    chat_sessions.insert(
                        ChannelChatKey::new(&binding.channel, &binding.chat_id),
                        record.id,
                    );
                    session_routes.insert(
                        record.id,
                        SessionChannelRoute::new(&binding.channel, &binding.chat_id),
                    );
                }
            }
        }

        Ok(Arc::new(Self {
            orchestrator,
            channels,
            chat_sessions: Mutex::new(chat_sessions),
            session_routes: Mutex::new(session_routes),
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
        // Transport path: the delivery mode is derived from the message's opaque
        // `extra_context` hint at this boundary (the transport layer never names a
        // `DeliveryKind`).
        let kind = DeliveryKind::from_extra_context(message.extra_context.as_deref());
        self.push_with_kind(message, kind).await
    }

    /// Push `message` with an explicit [`DeliveryKind`], bypassing
    /// `extra_context` derivation. Used by the `interrupt`/`cancel` facades, which
    /// already know the mode as a value.
    pub(crate) async fn push_with_kind(
        &self,
        message: InboundMessage,
        kind: DeliveryKind,
    ) -> Result<(), CapabilityError> {
        let session = self.resolve_session(&message)?;
        // A stand-alone control: callers of `push_message` drive serially and
        // cannot signal it mid-flight, but routing through `deliver_interruptible`
        // still honours the `DeliveryKind` (notably `Cancel`, which supersedes a
        // lingering task before this message starts fresh).
        let control = SessionControl::new();
        self.deliver_controlled(session, message, kind, &control)
            .await?;
        Ok(())
    }

    /// Resolve and record the reply route for `message`, returning its bound
    /// session. Separated so a concurrent driver can learn the target session
    /// before starting an interruptible drive.
    ///
    /// # Errors
    ///
    /// [`CapabilityError::InvalidArg`] for an ill-formed message,
    /// [`CapabilityError::NotFound`] when the channel is unknown or the chat is
    /// not bound to a session.
    pub fn resolve_session(&self, message: &InboundMessage) -> Result<SessionId, CapabilityError> {
        self.validate_inbound(message)?;
        let session = self.session_for(message)?;
        self.session_routes()
            .insert(session, SessionChannelRoute::from_inbound(message));
        Ok(session)
    }

    /// Deliver `message` to its (already resolved) `session` under an out-of-band
    /// [`SessionControl`], driving interruptibly and surfacing any output.
    /// Returns why the drive stopped.
    ///
    /// # Errors
    ///
    /// Maps [`DeliverError`] to [`CapabilityError`].
    pub async fn deliver_controlled(
        &self,
        session: SessionId,
        message: InboundMessage,
        kind: DeliveryKind,
        control: &SessionControl,
    ) -> Result<DriveStop, CapabilityError> {
        let (output, stop) = self
            .orchestrator
            .deliver_interruptible(session, session_message(message, kind), control)
            .await
            .map_err(map_deliver_error)?;
        self.surface_output(output)?;
        Ok(stop)
    }

    /// Continue a gracefully-interrupted `session`: record the interruption
    /// marker, then deliver `message` (the interrupting input) as the
    /// continuation, driving interruptibly and surfacing output.
    ///
    /// # Errors
    ///
    /// Maps [`DeliverError`] to [`CapabilityError`].
    pub async fn continue_interrupted(
        &self,
        session: SessionId,
        message: InboundMessage,
        control: &SessionControl,
    ) -> Result<DriveStop, CapabilityError> {
        // The interrupting input is delivered as the continuation; the
        // orchestrator's `continue_interrupted` ignores the delivery kind, so it
        // is appended.
        let (output, stop) = self
            .orchestrator
            .continue_interrupted(
                session,
                session_message(message, DeliveryKind::Append),
                control,
            )
            .await
            .map_err(map_deliver_error)?;
        self.surface_output(output)?;
        Ok(stop)
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

        // Record the binding onto the session record so this router (or another
        // one over the same orchestrator) can rebuild routes from `session_list`.
        // The session was just validated to exist, so this does not fail in
        // practice; a lost write is logged inside the store, not fatal here.
        self.orchestrator
            .session_set_binding(session, channel, chat_id)
            .map_err(|error| CapabilityError::Failed(error.to_string()))?;
        Ok(())
    }

    pub fn list_sessions(&self) -> Vec<SessionRecord> {
        let routes = self.session_routes();
        self.orchestrator
            .session_list()
            .into_iter()
            .map(|mut record| {
                if let Some(route) = routes.get(&record.id) {
                    record.binding = Some(SessionBinding::new(
                        route.channel.clone(),
                        route.chat_id.clone(),
                    ));
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

impl<F, H, Timer> ChannelRuntime for ChannelRouter<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn push_message(&self, message: InboundMessage) -> ChannelFuture<'_> {
        Box::pin(async move { ChannelRouter::push_message(self, message).await })
    }
}

/// Build the orchestrator's [`SessionMessage`] from an inbound transport
/// message, stamping the resolved [`DeliveryKind`](claw_core::DeliveryKind). The
/// kind is decided at this boundary (from `extra_context` on the transport path,
/// or forced by the caller), never carried on the transport message itself.
fn session_message(message: InboundMessage, kind: DeliveryKind) -> SessionMessage {
    SessionMessage::new(message.text, message.message_id, message.sender_id).with_kind(kind)
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

    use claw_api::{BackendKind, ClawApiConfig};
    use claw_capability::{Capability, Registry};
    use claw_core::agent::MapAgentResolver;
    use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp};

    use super::*;

    type TestOrchestrator =
        Orchestrator<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;

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

    fn web_registry() -> Registry {
        let registry = Registry::new();
        registry
            .register(Capability::channel(Arc::new(TestChannel)))
            .unwrap();
        registry
    }

    fn test_orchestrator() -> Arc<TestOrchestrator> {
        let llm_config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        );
        Orchestrator::<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>::new(
            Arc::new(MapAgentResolver::new()),
            llm_config,
            "/channel-router-test",
        )
        .unwrap()
    }

    #[test]
    fn bindings_rebuild_from_orchestrator_session_list() {
        let orchestrator = test_orchestrator();
        let first_router = ChannelRouter::new(Arc::clone(&orchestrator), &web_registry()).unwrap();
        let sid = first_router.new_session();
        first_router.bind_session(sid, "web", "chat-1").unwrap();

        // A fresh router over the same orchestrator rebuilds its routing tables
        // from the orchestrator's session records.
        let router = ChannelRouter::new(orchestrator, &web_registry()).unwrap();

        // The binding was rebuilt: the session reappears with its channel/chat.
        let sessions = router.list_sessions();
        assert_eq!(sessions.len(), 1);
        let record = sessions.first().unwrap();
        assert_eq!(record.id, sid);
        assert_eq!(
            record.binding.as_ref(),
            Some(&SessionBinding::new("web", "chat-1"))
        );
    }

    #[test]
    fn unbound_inbound_does_not_create_session() {
        let registry = Registry::new();
        registry
            .register(Capability::channel(Arc::new(TestChannel)))
            .unwrap();
        let orchestrator = test_orchestrator();
        let router = ChannelRouter::new(orchestrator, &registry).unwrap();
        let existing = router.new_session();

        let error = block_on(router.push_message(InboundMessage {
            message_id: "m1".into(),
            channel: "web".into(),
            chat_id: "chat".into(),
            sender_id: None,
            text: "hello".into(),
            ..Default::default()
        }))
        .unwrap_err();

        assert_eq!(error, CapabilityError::NotFound);
        let sessions = router.list_sessions();
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions.first().unwrap().id, existing);
    }
}
