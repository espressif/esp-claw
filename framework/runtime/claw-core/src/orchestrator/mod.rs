//! Layer 1 orchestrator: session registry, ingress, egress reply routing.
//!
//! Inbound logic lives in [`Orchestrator::on_user_message`] and
//! [`Orchestrator::on_command`].

mod instance;

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use claw_context::Block;

use crate::agent::factory::AgentFactory;
use crate::agent::registry::AgentIdAllocator;
use crate::channels::{
    ChannelEgress, ChannelIngressSink, Command, InboundCommand, InboundMessage, IngressFuture,
};
use crate::session::{
    DeliverError, SessionError, SessionId, SessionMessage, SessionOut, SessionRoutes, SessionStore,
};

use self::instance::{DriveOutput, OrchestratorInstance};

pub struct Orchestrator {
    egress: Arc<dyn ChannelEgress>,
    /// Builds agents for every session's registry. Required at construction
    /// (enforced by the builder typestate): the orchestrator owns no LLM client
    /// of its own — the factory holds whatever an agent needs to run.
    factory: Arc<dyn AgentFactory>,
    /// Global agent-id allocator shared by every per-session registry so ids are
    /// unique across the whole process, not merely within one session.
    next_agent_id: AgentIdAllocator,
    /// One isolated agent graph per session. The map lock is held only while an
    /// instance is inserted, removed, or taken for driving; it is not held while
    /// the agent graph awaits LLM/tool work.
    instances: Mutex<HashMap<SessionId, OrchestratorInstance>>,
    sessions: SessionStore,
    routes: SessionRoutes,
    /// Process-wide (Global scope) prose injected into every session's agents.
    /// Shared as an `Arc<[Block]>` so all sessions reference one computed set for
    /// byte-identical prefixes. Empty until a Global scope provider populates it.
    global_context: Arc<[Block<'static>]>,
}

impl Orchestrator {
    // -----------------------------------------------------------------------
    // Inbound callbacks — edit these
    // -----------------------------------------------------------------------

    async fn on_user_message(&self, session_id: SessionId, msg: &SessionMessage) {
        let mut instance = self.take_instance(session_id);
        let output = {
            let turn = instance.next_turn();
            // session > turn: the session span opens `conversation.session`, the
            // turn span opens `conversation.turn`. Every agent/iteration/tool span
            // produced while driving nests under them, so one drive reads as a unit.
            let _session_span =
                tracing::info_span!("session", conversation.session = %session_id).entered();
            let _turn_span = tracing::info_span!(
                "turn",
                conversation.turn = turn,
                message_id = %msg.message_id,
                cause = "message"
            )
            .entered();
            if let Err(error) = instance.deliver(msg.text.clone()) {
                tracing::warn!(session = %session_id, %error, "failed to build/deliver root");
                self.put_instance(session_id, instance);
                return;
            }
            instance.drive().await
        };

        self.put_instance(session_id, instance);
        self.surface_output(output);
    }

    /// Move the session's agent graph out of the map so it can be driven without
    /// holding the map lock across `.await`.
    fn take_instance(&self, session_id: SessionId) -> OrchestratorInstance {
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id)
            .unwrap_or_else(|| {
                OrchestratorInstance::new(
                    session_id,
                    Arc::clone(&self.factory),
                    self.next_agent_id.clone(),
                    Arc::clone(&self.global_context),
                )
            })
    }

    fn put_instance(&self, session_id: SessionId, instance: OrchestratorInstance) {
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .insert(session_id, instance);
    }

    /// Route a [`DriveOutput`] to the session's egress: replies as messages, and
    /// each pending approval as a visible message tagged with its agent/approval
    /// id. Resolving an approval is an internal concern; there is no public
    /// resolve entry point on the orchestrator yet.
    fn surface_output(&self, output: DriveOutput) {
        for reply in output.replies {
            if let Err(error) = self.send_message(reply.session, reply.text) {
                tracing::warn!(session = %reply.session, %error, "failed to send reply");
            }
        }
        for approval in output.approvals {
            // There is no inbound approval-response binding yet; surface the
            // request so it is visible and resolvable out of band rather than
            // dropped. The id tags let a caller target the exact agent.
            tracing::info!(
                session = %approval.session,
                agent = %approval.agent,
                approval = %approval.approval,
                "approval requested"
            );
            let text = format!(
                "[approval needed · {} · {}] {}",
                approval.agent, approval.approval, approval.summary
            );
            if let Err(error) = self.send_message(approval.session, text) {
                tracing::warn!(session = %approval.session, %error, "failed to surface approval");
            }
        }
    }

    fn on_command(&self, session_id: SessionId, cmd: &Command) {
        // Not wired yet (intentionally a no-op, not a panic): the `Command`
        // command flow still uses the legacy task/run model and must be
        // reconciled with the session/agent/approval model before it can drive the graph.
        // Until then, acknowledge in the log and drop rather than aborting.
        tracing::warn!(session = %session_id, command = ?cmd, "inbound command ignored (not implemented yet)");
    }

    fn send_message(
        &self,
        session_id: SessionId,
        text: impl Into<String>,
    ) -> Result<(), DeliverError> {
        let reply_route = self
            .routes
            .get(session_id)
            .ok_or(DeliverError::NoReplyRoute(session_id))?;
        SessionOut::new(self.egress.as_ref(), &reply_route)
            .send_message(text)
            .map_err(Into::into)
    }

    async fn deliver_user_message(&self, msg: InboundMessage) -> Result<(), DeliverError> {
        if msg.session_id.trim().is_empty() {
            return Err(DeliverError::MissingSessionId);
        }
        let session_id = SessionId::from_wire(&msg.session_id)
            .map_err(|_| DeliverError::InvalidSessionId(msg.session_id.clone()))?;
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }

        self.routes.update_from_inbound(session_id, &msg);
        let session_msg = SessionMessage::from_inbound(&msg);
        self.on_user_message(session_id, &session_msg).await;
        Ok(())
    }

    fn deliver_command(&self, inbound: InboundCommand) -> Result<(), DeliverError> {
        let session_id = inbound.session_id;
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }
        if self.routes.get(session_id).is_none() {
            return Err(DeliverError::NoReplyRoute(session_id));
        }
        self.on_command(session_id, &inbound.command);
        Ok(())
    }

    pub fn session_create(&self) -> SessionId {
        self.sessions.create().id
    }

    pub fn session_delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        self.sessions.delete(session_id)?;
        self.routes.remove(session_id);
        // Drop the session's agent graph so a deleted session leaves no live
        // agents behind.
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id);
        Ok(())
    }

    pub fn builder() -> OrchestratorBuilder<ChannelsUnset, FactoryUnset> {
        OrchestratorBuilder {
            channels: ChannelsUnset,
            factory: FactoryUnset,
            global_context: Arc::from([]),
        }
    }
}

impl ChannelIngressSink for Orchestrator {
    fn push_user_message(&self, msg: InboundMessage) -> IngressFuture<'_> {
        Box::pin(async move {
            if let Err(err) = self.deliver_user_message(msg).await {
                tracing::warn!(error = %err, "ingress user message deliver failed");
            }
        })
    }

    fn push_command(&self, command: InboundCommand) -> IngressFuture<'_> {
        Box::pin(async move {
            if let Err(err) = self.deliver_command(command) {
                tracing::warn!(error = %err, "ingress command deliver failed");
            }
        })
    }
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

pub struct OrchestratorBuilder<Channels, Factory> {
    channels: Channels,
    factory: Factory,
    /// Process-wide (Global scope) prose injected into every agent. Optional;
    /// defaults to empty. Carried through the typestate transitions so it can be
    /// set before either required dependency.
    global_context: Arc<[Block<'static>]>,
}

impl<Channels, Factory> OrchestratorBuilder<Channels, Factory> {
    /// Inject the Global-scope prose blocks shared by every session's agents.
    ///
    /// Optional (defaults to empty). Shared as an `Arc<[Block]>` so all agents
    /// reference one computed set for byte-identical prefixes.
    pub fn with_global_context(mut self, blocks: Arc<[Block<'static>]>) -> Self {
        self.global_context = blocks;
        self
    }
}

impl<Channels> OrchestratorBuilder<Channels, FactoryUnset> {
    /// Inject the [`AgentFactory`] used to build every session's agents.
    ///
    /// Required: [`build`](OrchestratorBuilder::build) is only callable once a
    /// factory is set, so the orchestrator can always materialize a root agent
    /// for a live session.
    pub fn with_agent_factory(
        self,
        factory: Arc<dyn AgentFactory>,
    ) -> OrchestratorBuilder<Channels, FactorySet> {
        OrchestratorBuilder {
            channels: self.channels,
            factory: FactorySet { factory },
            global_context: self.global_context,
        }
    }
}

// Typestate markers for `OrchestratorBuilder`. They are `pub` only because they
// appear in the builder's public method signatures; they carry no usable API and
// are hidden from the rendered docs.
#[doc(hidden)]
pub struct ChannelsUnset;
#[doc(hidden)]
pub struct ChannelsEgressOnly {
    egress: Arc<dyn ChannelEgress>,
}
#[doc(hidden)]
pub struct FactoryUnset;
#[doc(hidden)]
pub struct FactorySet {
    factory: Arc<dyn AgentFactory>,
}

impl<Factory> OrchestratorBuilder<ChannelsUnset, Factory> {
    /// Inject the [`ChannelEgress`] outbound messages are routed through.
    ///
    /// Required: [`build`](OrchestratorBuilder::build) is only callable once an
    /// egress is set, so every reply has somewhere to go.
    pub fn config_egress(
        self,
        egress: Arc<dyn ChannelEgress>,
    ) -> OrchestratorBuilder<ChannelsEgressOnly, Factory> {
        OrchestratorBuilder {
            channels: ChannelsEgressOnly { egress },
            factory: self.factory,
            global_context: self.global_context,
        }
    }
}

impl OrchestratorBuilder<ChannelsEgressOnly, FactorySet> {
    pub fn build(self) -> Arc<Orchestrator> {
        Arc::new(Orchestrator {
            egress: self.channels.egress,
            factory: self.factory.factory,
            next_agent_id: AgentIdAllocator::new(),
            instances: Mutex::new(HashMap::new()),
            sessions: SessionStore::new(),
            routes: SessionRoutes::new(),
            global_context: self.global_context,
        })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::collections::VecDeque;
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use super::*;
    use crate::agent::factory::AgentFactory;
    use crate::agent::{
        Agent, AgentCommand, AgentCommandError, AgentId, AgentKind, AgentTickFuture, ApprovalId,
        GraphHost, TickOutcome,
    };
    use crate::channels::{ChannelEgressHub, ChannelTransport, RecordingTransport};

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

    // -- A fake factory + agent that echoes each delivered message --------------

    /// A scripted agent: every message it receives is echoed back as
    /// `echo:<message>` on the next tick, then it goes idle.
    struct EchoAgent {
        id: AgentId,
        pending: VecDeque<String>,
    }

    impl Agent for EchoAgent {
        fn id(&self) -> AgentId {
            self.id
        }

        fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
            if let AgentCommand::AppendMessage(message) = command {
                self.pending.push_back(message);
            }
            Ok(())
        }

        fn deliver_child_result(&mut self, _child: AgentId, _text: String, _ok: bool) {}

        fn tick(&mut self) -> AgentTickFuture<'_> {
            let outcome = match self.pending.pop_front() {
                Some(message) => TickOutcome::Yielded {
                    text: format!("echo:{message}"),
                },
                None => TickOutcome::Idle,
            };
            Box::pin(async move { outcome })
        }
    }

    /// Builds [`EchoAgent`]s seeded with the goal as their first message.
    struct EchoFactory;

    impl AgentFactory for EchoFactory {
        fn create_agent(
            &self,
            id: AgentId,
            _kind: &AgentKind,
            goal: String,
            _host: Arc<dyn GraphHost>,
            _is_root: bool,
            _inherited_context: Arc<[Block<'static>]>,
        ) -> Result<Box<dyn Agent>, String> {
            Ok(Box::new(EchoAgent {
                id,
                pending: VecDeque::from([goal]),
            }))
        }
    }

    /// An agent that parks on an approval the first time it ticks, then yields
    /// once the decision arrives — to exercise the approval round-trip.
    struct ApprovalAgent {
        id: AgentId,
        asked: bool,
        approved: bool,
        done: bool,
    }

    impl Agent for ApprovalAgent {
        fn id(&self) -> AgentId {
            self.id
        }

        fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
            if matches!(command, AgentCommand::ApprovalResult { .. }) {
                self.approved = true;
            }
            Ok(())
        }

        fn deliver_child_result(&mut self, _child: AgentId, _text: String, _ok: bool) {}

        fn tick(&mut self) -> AgentTickFuture<'_> {
            let outcome = if !self.asked {
                self.asked = true;
                TickOutcome::AwaitingApproval {
                    id: ApprovalId(1),
                    summary: "ok?".into(),
                }
            } else if self.approved && !self.done {
                self.done = true;
                TickOutcome::Yielded {
                    text: "approved-done".into(),
                }
            } else {
                TickOutcome::Idle
            };
            Box::pin(async move { outcome })
        }
    }

    struct ApprovalFactory;

    impl AgentFactory for ApprovalFactory {
        fn create_agent(
            &self,
            id: AgentId,
            _kind: &AgentKind,
            _goal: String,
            _host: Arc<dyn GraphHost>,
            _is_root: bool,
            _inherited_context: Arc<[Block<'static>]>,
        ) -> Result<Box<dyn Agent>, String> {
            Ok(Box::new(ApprovalAgent {
                id,
                asked: false,
                approved: false,
                done: false,
            }))
        }
    }

    fn user_msg(session_id: SessionId, text: &str) -> InboundMessage {
        InboundMessage {
            message_id: "m1".into(),
            channel: "qq".into(),
            chat_id: "chat-a".into(),
            sender_id: None,
            session_id: session_id.to_wire(),
            text: text.into(),
        }
    }

    fn orchestrator_with_factory(
        factory: Arc<dyn AgentFactory>,
    ) -> (Arc<Orchestrator>, Arc<RecordingTransport>) {
        let transport = RecordingTransport::new("qq");
        let egress = Arc::new(ChannelEgressHub::new());
        let as_transport: Arc<dyn ChannelTransport> = Arc::clone(&transport) as Arc<_>;
        egress.register(as_transport);

        let orch = Orchestrator::builder()
            .config_egress(egress as Arc<dyn ChannelEgress>)
            .with_agent_factory(factory)
            .build();
        (orch, transport)
    }

    #[test]
    fn user_message_drives_root_and_replies() {
        let (orch, transport) = orchestrator_with_factory(Arc::new(EchoFactory));
        let session = orch.session_create();

        block_on(orch.push_user_message(user_msg(session, "hi")));

        let sent = transport.drain_sent();
        assert_eq!(sent.len(), 1);
        assert_eq!(sent[0].text, "echo:hi");
    }

    #[test]
    fn second_message_reuses_the_same_session_root() {
        let (orch, transport) = orchestrator_with_factory(Arc::new(EchoFactory));
        let session = orch.session_create();

        block_on(orch.push_user_message(user_msg(session, "first")));
        block_on(orch.push_user_message(user_msg(session, "second")));

        let sent = transport.drain_sent();
        assert_eq!(
            sent.iter().map(|m| m.text.clone()).collect::<Vec<_>>(),
            vec!["echo:first".to_string(), "echo:second".to_string()]
        );
        // Exactly one instance was created for the session.
        assert_eq!(orch.instances.lock().unwrap().len(), 1);
    }

    #[test]
    fn root_approval_is_surfaced_as_a_message() {
        let (orch, transport) = orchestrator_with_factory(Arc::new(ApprovalFactory));
        let session = orch.session_create();

        // The first message parks the root on an approval, surfaced as a message.
        // (Resolving an approval is an internal concern — there is no public
        // resolve entry point on the orchestrator.)
        block_on(orch.push_user_message(user_msg(session, "do it")));
        let surfaced = transport.drain_sent();
        assert_eq!(surfaced.len(), 1);
        assert!(
            surfaced[0].text.contains("[approval needed"),
            "expected an approval surface, got: {}",
            surfaced[0].text
        );
    }

    #[test]
    fn two_sessions_have_independent_graphs() {
        let (orch, transport) = orchestrator_with_factory(Arc::new(EchoFactory));
        let s1 = orch.session_create();
        let s2 = orch.session_create();

        block_on(orch.push_user_message(user_msg(s1, "one")));
        block_on(orch.push_user_message(user_msg(s2, "two")));

        // One isolated instance per session.
        assert_eq!(orch.instances.lock().unwrap().len(), 2);
        let texts: Vec<String> = transport.drain_sent().into_iter().map(|m| m.text).collect();
        assert!(texts.contains(&"echo:one".to_string()));
        assert!(texts.contains(&"echo:two".to_string()));
    }
}
