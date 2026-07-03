//! Layer 1 orchestrator: session registry and per-session agent graph driving.
//!
//! Channel routing is owned by the layer above this crate.

mod control;
mod instance;

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use claw_api::ClawApiConfig;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    AgentIdAllocator, AgentResolver, CancelReason, FsAgentFactory, FsAgentFactoryError,
};
use crate::session::{
    DeliverError, DeliveryKind, SessionError, SessionId, SessionMessage, SessionRecord,
    SessionStore,
};

pub use self::control::{DriveStop, SessionControl};
pub use self::instance::{ApprovalRequest, DriveOutput, RootReply};

use self::instance::OrchestratorInstance;

/// RAII checkout of a session's [`OrchestratorInstance`]: holds the instance out
/// of the map while it is driven and reinserts it on drop, so no exit path (an
/// early `?`, a panic while driving, or normal return) can drop the graph.
struct InstanceSlot<'a, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    orchestrator: &'a Orchestrator<F, H, Timer>,
    session_id: SessionId,
    instance: Option<OrchestratorInstance<F, H, Timer>>,
}

impl<F, H, Timer> InstanceSlot<'_, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn get_mut(&mut self) -> &mut OrchestratorInstance<F, H, Timer> {
        // Invariant: `instance` is `Some` for the whole lifetime of the slot;
        // it is only taken in `Drop`, after which the slot is unreachable.
        self.instance
            .as_mut()
            .expect("InstanceSlot holds its instance until Drop")
    }
}

impl<F, H, Timer> Drop for InstanceSlot<'_, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn drop(&mut self) {
        if let Some(instance) = self.instance.take() {
            self.orchestrator.put_instance(self.session_id, instance);
        }
    }
}

pub struct Orchestrator<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Builds agents for every session's registry. Required at construction
    /// time: the orchestrator owns no LLM client of its own — the factory holds
    /// whatever an agent needs to run.
    factory: Arc<FsAgentFactory<F, H, Timer>>,
    /// Global agent-id allocator shared by every per-session registry so ids are
    /// unique across the whole process, not merely within one session.
    next_agent_id: AgentIdAllocator,
    /// One isolated agent graph per session. The map lock is held only while an
    /// instance is inserted, removed, or taken for driving; it is not held while
    /// the agent graph awaits LLM/tool work.
    instances: Mutex<HashMap<SessionId, OrchestratorInstance<F, H, Timer>>>,
    sessions: SessionStore,
}

impl<F, H, Timer> Orchestrator<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build an orchestrator and its concrete filesystem-backed agent factory.
    ///
    /// `resolver` maps manifest-declared capability names to handlers,
    /// `llm_config` is cloned into every agent, and `persistence_dir` is the
    /// storage root the factory owns below this orchestrator.
    ///
    /// # Errors
    ///
    /// Returns [`FsAgentFactoryError`] when the factory cannot be assembled.
    pub fn new(
        resolver: Arc<dyn AgentResolver>,
        llm_config: ClawApiConfig,
        persistence_dir: &str,
    ) -> Result<Arc<Self>, FsAgentFactoryError> {
        let factory = Arc::new(FsAgentFactory::<F, H, Timer>::new(
            resolver,
            llm_config,
            persistence_dir,
        )?);
        Ok(Arc::new(Self {
            factory,
            next_agent_id: AgentIdAllocator::new(),
            instances: Mutex::new(HashMap::new()),
            sessions: SessionStore::new(),
        }))
    }

    /// Deliver one user message to a live session and drive that session's agent
    /// graph until it has no ready work.
    ///
    /// The returned [`DriveOutput`] is intentionally channel-free. The caller is
    /// responsible for routing replies and approvals to whatever transport
    /// delivered the message.
    pub async fn deliver(
        &self,
        session_id: SessionId,
        msg: SessionMessage,
    ) -> Result<DriveOutput, DeliverError> {
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }

        // The instance is checked out of the map so it can be driven without
        // holding the map lock across `.await`. `InstanceSlot` is an RAII guard:
        // it reinserts the (possibly mutated) instance on every exit path — the
        // `?` below, a `drive().await` panic, or normal return — so a session's
        // agent graph is never silently dropped.
        //
        // Delivery is assumed to be serialized per session by the driving layer
        // (one agent executor). Two concurrent `deliver`s for the same session
        // would each check out a slot and the last reinsert would win; the
        // channel router does not currently issue such concurrent calls.
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();
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
        instance
            .deliver(msg.text.clone())
            .map_err(DeliverError::Agent)?;
        Ok(instance.drive().await)
    }

    /// Deliver `msg` and drive the session, but observe an out-of-band
    /// [`SessionControl`] so a concurrent task can interrupt or cancel the drive
    /// while it is in flight.
    ///
    /// The message's [`DeliveryKind`] shapes how the *starting* delivery treats a
    /// task from a previous drive: [`DeliveryKind::Cancel`] supersedes it (a
    /// cancellation marker is recorded and the task is reset) before this message
    /// starts a fresh one; [`DeliveryKind::Append`] and [`DeliveryKind::Interrupt`]
    /// just append (there is nothing running to interrupt at the boundary).
    ///
    /// In-flight `Interrupt`/`Cancel` — a message arriving *while this drive is
    /// running* — is delivered by the caller through `control`, and its
    /// continuation is handled with [`continue_interrupted`](Self::continue_interrupted)
    /// (interrupt) or another `deliver_interruptible` with a `Cancel` kind
    /// (cancel).
    ///
    /// # Errors
    ///
    /// [`DeliverError::SessionNotFound`] if the session is not registered, or
    /// [`DeliverError::Agent`] if the root agent cannot be built.
    pub async fn deliver_interruptible(
        &self,
        session_id: SessionId,
        msg: SessionMessage,
        control: &SessionControl,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();
        let turn = instance.next_turn();
        let _session_span =
            tracing::info_span!("session", conversation.session = %session_id).entered();
        let _turn_span = tracing::info_span!(
            "turn",
            conversation.turn = turn,
            message_id = %msg.message_id,
            cause = "message"
        )
        .entered();
        // A Cancel-kind delivery supersedes any lingering task before this
        // message starts a fresh one. `cancel_root` and the following `deliver`
        // land on the root's inbox in order, so the next drive commits the
        // cancellation marker and then starts the new task.
        if msg.kind == DeliveryKind::Cancel {
            instance.cancel_root(CancelReason::Superseded);
        }
        instance
            .deliver(msg.text.clone())
            .map_err(DeliverError::Agent)?;
        Ok(instance.drive_interruptible(control).await)
    }

    /// Continue a session whose in-flight drive was gracefully interrupted:
    /// record the interruption marker, deliver `msg` (the interrupting input) as
    /// the continuation, and drive again (interruptibly) so the still-alive task
    /// re-decides with the new input.
    ///
    /// # Errors
    ///
    /// [`DeliverError::SessionNotFound`] if the session is not registered, or
    /// [`DeliverError::Agent`] if delivery fails.
    pub async fn continue_interrupted(
        &self,
        session_id: SessionId,
        msg: SessionMessage,
        control: &SessionControl,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();
        let turn = instance.next_turn();
        let _session_span =
            tracing::info_span!("session", conversation.session = %session_id).entered();
        let _turn_span = tracing::info_span!(
            "turn",
            conversation.turn = turn,
            message_id = %msg.message_id,
            cause = "interrupt-continue"
        )
        .entered();
        instance.mark_interrupted();
        instance
            .deliver(msg.text.clone())
            .map_err(DeliverError::Agent)?;
        Ok(instance.drive_interruptible(control).await)
    }

    /// Check the session's agent graph out of the map (building a fresh instance
    /// when the session has none yet), wrapped in an [`InstanceSlot`] that
    /// reinserts it on drop.
    fn checkout_instance(&self, session_id: SessionId) -> InstanceSlot<'_, F, H, Timer> {
        let instance = self
            .instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id)
            .unwrap_or_else(|| {
                OrchestratorInstance::new(
                    session_id,
                    Arc::clone(&self.factory),
                    self.next_agent_id.clone(),
                )
            });
        InstanceSlot {
            orchestrator: self,
            session_id,
            instance: Some(instance),
        }
    }

    fn put_instance(&self, session_id: SessionId, instance: OrchestratorInstance<F, H, Timer>) {
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .insert(session_id, instance);
    }

    pub fn session_create(&self) -> SessionId {
        self.sessions.create().id
    }

    pub fn session_list(&self) -> Vec<SessionRecord> {
        let mut sessions = self.sessions.list();
        sessions.sort_by_key(|record| record.id.0);
        sessions
    }

    pub fn session_exists(&self, session_id: SessionId) -> bool {
        self.sessions.contains(session_id)
    }

    /// Persist `session_id`'s channel binding so it can be rebuilt after a
    /// restart. See [`SessionStore::set_binding`].
    ///
    /// # Errors
    ///
    /// [`SessionError::NotFound`] when `session_id` is not registered.
    pub fn session_set_binding(
        &self,
        session_id: SessionId,
        channel: impl Into<String>,
        chat_id: impl Into<String>,
    ) -> Result<(), SessionError> {
        self.sessions.set_binding(session_id, channel, chat_id)
    }

    pub fn session_delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        self.sessions.delete(session_id)?;
        // Drop the session's agent graph so a deleted session leaves no live
        // agents behind.
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id);
        Ok(())
    }
}

#[cfg(test)]
#[cfg(any())]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::collections::VecDeque;
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use claw_context::Block;

    use super::*;
    use crate::agent::{
        Agent, AgentAbortHandle, AgentCommand, AgentCommandError, AgentFactory, AgentId, AgentKind,
        AgentPlacement, AgentTickFuture, ApprovalId, GraphHost, TickOutcome,
    };

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

        fn abort_handle(&self) -> AgentAbortHandle {
            AgentAbortHandle::default()
        }

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
            _placement: AgentPlacement,
            _host: Arc<dyn GraphHost>,
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

        fn abort_handle(&self) -> AgentAbortHandle {
            AgentAbortHandle::default()
        }

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
            _placement: AgentPlacement,
            _host: Arc<dyn GraphHost>,
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

    fn user_msg(text: &str) -> SessionMessage {
        SessionMessage::new(text, "m1", None)
    }

    fn orchestrator_with_factory(factory: Arc<dyn AgentFactory>) -> Arc<Orchestrator> {
        Orchestrator::new(factory)
    }

    #[test]
    fn user_message_drives_root_and_replies() {
        let orch = orchestrator_with_factory(Arc::new(EchoFactory));
        let session = orch.session_create();

        let output = block_on(orch.deliver(session, user_msg("hi"))).unwrap();

        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "echo:hi");
    }

    #[test]
    fn deliver_interruptible_append_matches_plain_deliver() {
        let orch = orchestrator_with_factory(Arc::new(EchoFactory));
        let session = orch.session_create();

        let control = SessionControl::new();
        let (output, stop) =
            block_on(orch.deliver_interruptible(session, user_msg("hi"), &control)).unwrap();

        // With no out-of-band signal, an Append delivery drives to quiescence just
        // like `deliver`.
        assert_eq!(stop, DriveStop::Quiescent);
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "echo:hi");
    }

    #[test]
    fn deliver_interruptible_cancel_kind_supersedes_before_delivering() {
        let orch = orchestrator_with_factory(Arc::new(EchoFactory));
        let session = orch.session_create();

        // Seed a task, then deliver a Cancel-kind message: it supersedes the prior
        // task and starts fresh from the new text.
        let _ = block_on(orch.deliver(session, user_msg("first"))).unwrap();
        let control = SessionControl::new();
        let (output, stop) = block_on(orch.deliver_interruptible(
            session,
            user_msg("second").with_kind(DeliveryKind::Cancel),
            &control,
        ))
        .unwrap();

        assert_eq!(stop, DriveStop::Quiescent);
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "echo:second");
    }

    #[test]
    fn second_message_reuses_the_same_session_root() {
        let orch = orchestrator_with_factory(Arc::new(EchoFactory));
        let session = orch.session_create();

        let first = block_on(orch.deliver(session, user_msg("first"))).unwrap();
        let second = block_on(orch.deliver(session, user_msg("second"))).unwrap();

        assert_eq!(
            first
                .replies
                .into_iter()
                .chain(second.replies)
                .map(|reply| reply.text)
                .collect::<Vec<_>>(),
            vec!["echo:first".to_string(), "echo:second".to_string()]
        );
        // Exactly one instance was created for the session.
        assert_eq!(orch.instances.lock().unwrap().len(), 1);
    }

    #[test]
    fn root_approval_is_surfaced_as_a_message() {
        let orch = orchestrator_with_factory(Arc::new(ApprovalFactory));
        let session = orch.session_create();

        // The first message parks the root on an approval, surfaced in
        // DriveOutput for the channel router.
        // (Resolving an approval is an internal concern — there is no public
        // resolve entry point on the orchestrator.)
        let output = block_on(orch.deliver(session, user_msg("do it"))).unwrap();
        assert_eq!(output.approvals.len(), 1);
        assert_eq!(output.approvals[0].summary, "ok?");
    }

    #[test]
    fn two_sessions_have_independent_graphs() {
        let orch = orchestrator_with_factory(Arc::new(EchoFactory));
        let s1 = orch.session_create();
        let s2 = orch.session_create();

        let one = block_on(orch.deliver(s1, user_msg("one"))).unwrap();
        let two = block_on(orch.deliver(s2, user_msg("two"))).unwrap();

        // One isolated instance per session.
        assert_eq!(orch.instances.lock().unwrap().len(), 2);
        let texts: Vec<String> = one
            .replies
            .into_iter()
            .chain(two.replies)
            .map(|reply| reply.text)
            .collect();
        assert!(texts.contains(&"echo:one".to_string()));
        assert!(texts.contains(&"echo:two".to_string()));
    }
}
