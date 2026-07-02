//! Layer 1 orchestrator: session registry and per-session agent graph driving.
//!
//! Channel routing is owned by the layer above this crate.

mod instance;

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use claw_context::Block;

use crate::agent::factory::AgentFactory;
use crate::agent::registry::AgentIdAllocator;
use crate::session::{
    DeliverError, SessionError, SessionId, SessionMessage, SessionRecord, SessionStore,
};

pub use self::instance::{ApprovalRequest, DriveOutput, RootReply};

use self::instance::OrchestratorInstance;

pub struct Orchestrator {
    /// Builds agents for every session's registry. Required at construction
    /// time: the orchestrator owns no LLM client of its own — the factory holds
    /// whatever an agent needs to run.
    factory: Arc<dyn AgentFactory>,
    /// Global agent-id allocator shared by every per-session registry so ids are
    /// unique across the whole process, not merely within one session.
    next_agent_id: AgentIdAllocator,
    /// One isolated agent graph per session. The map lock is held only while an
    /// instance is inserted, removed, or taken for driving; it is not held while
    /// the agent graph awaits LLM/tool work.
    instances: Mutex<HashMap<SessionId, OrchestratorInstance>>,
    sessions: SessionStore,
    /// Process-wide (Global scope) prose injected into every session's agents.
    /// Shared as an `Arc<[Block]>` so all sessions reference one computed set for
    /// byte-identical prefixes. Empty until a Global scope provider populates it.
    global_context: Arc<[Block<'static>]>,
}

impl Orchestrator {
    /// Build an orchestrator using `factory` for each session's agent graph.
    pub fn new(factory: Arc<dyn AgentFactory>) -> Arc<Self> {
        Self::with_global_context(factory, Arc::from([]))
    }

    /// Build an orchestrator with process-wide Global-scope prose blocks.
    pub fn with_global_context(
        factory: Arc<dyn AgentFactory>,
        global_context: Arc<[Block<'static>]>,
    ) -> Arc<Self> {
        Arc::new(Self {
            factory,
            next_agent_id: AgentIdAllocator::new(),
            instances: Mutex::new(HashMap::new()),
            sessions: SessionStore::new(),
            global_context,
        })
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
            instance
                .deliver(msg.text.clone())
                .map_err(DeliverError::Agent)?;
            instance.drive().await
        };

        self.put_instance(session_id, instance);
        Ok(output)
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
