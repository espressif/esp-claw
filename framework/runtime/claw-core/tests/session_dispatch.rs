//! Session isolation and delivery validation tests.

use core::future::Future;
use core::task::{Context, Poll};
use std::sync::Arc;
use std::task::{Wake, Waker};

use claw_context::Block;
use claw_core::agent::{
    Agent, AgentCommand, AgentCommandError, AgentFactory, AgentId, AgentKind, AgentTickFuture,
    GraphHost, TickOutcome,
};
use claw_core::{Orchestrator, SessionId, SessionMessage};

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

/// An agent that never produces output. These tests only exercise delivery
/// validation (no message reaches a live session's graph), so the factory below
/// is never actually invoked — but the builder requires one.
struct IdleAgent {
    id: AgentId,
}

impl Agent for IdleAgent {
    fn id(&self) -> AgentId {
        self.id
    }

    fn send_command(&mut self, _command: AgentCommand) -> Result<(), AgentCommandError> {
        Ok(())
    }

    fn deliver_child_result(&mut self, _child: AgentId, _text: String, _ok: bool) {}

    fn tick(&mut self) -> AgentTickFuture<'_> {
        Box::pin(async { TickOutcome::Idle })
    }
}

struct IdleFactory;

impl AgentFactory for IdleFactory {
    fn create_agent(
        &self,
        id: AgentId,
        _kind: &AgentKind,
        _goal: String,
        _host: Arc<dyn GraphHost>,
        _is_root: bool,
        _inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Box<dyn Agent>, String> {
        Ok(Box::new(IdleAgent { id }))
    }
}

fn test_orchestrator() -> Arc<Orchestrator> {
    Orchestrator::new(Arc::new(IdleFactory))
}

fn user_msg(text: &str) -> SessionMessage {
    SessionMessage::new(text, "m1", None)
}

#[test]
fn sessions_can_be_created_independently() {
    let orch = test_orchestrator();

    let s1 = orch.session_create();
    let s2 = orch.session_create();
    assert_ne!(s1, s2);
}

#[test]
fn delete_session_rejects_deliver() {
    let orch = test_orchestrator();

    let sid = orch.session_create();
    orch.session_delete(sid).unwrap();

    assert!(block_on(orch.deliver(sid, user_msg("ghost"))).is_err());
}

#[test]
fn deliver_with_unknown_session_id_is_rejected() {
    let orch = test_orchestrator();

    assert!(block_on(orch.deliver(SessionId(99), user_msg("orphan"))).is_err());
}
