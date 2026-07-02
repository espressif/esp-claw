//! Session isolation and ingress validation tests.

use core::future::Future;
use core::task::{Context, Poll};
use std::sync::Arc;
use std::task::{Wake, Waker};

use claw_context::Block;
use claw_core::agent::{
    Agent, AgentCommand, AgentCommandError, AgentFactory, AgentId, AgentKind, AgentTickFuture,
    GraphHost, TickOutcome,
};
use claw_core::{
    ChannelEgressHub, ChannelIngressSink, InboundMessage, Orchestrator, RecordingTransport,
    SessionId,
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

/// An agent that never produces output. These tests only exercise ingress
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

fn test_orchestrator() -> (Arc<Orchestrator>, Arc<RecordingTransport>) {
    let transport = RecordingTransport::new("qq");
    let for_drain = Arc::clone(&transport);
    let egress = Arc::new(ChannelEgressHub::new());
    let as_transport: Arc<dyn claw_core::ChannelTransport> = transport;
    egress.register(as_transport);

    let orch = dummy_orchestrator(egress);
    (orch, for_drain)
}

fn dummy_orchestrator(egress: Arc<dyn claw_core::ChannelEgress>) -> Arc<Orchestrator> {
    Orchestrator::builder()
        .config_egress(egress)
        .with_agent_factory(Arc::new(IdleFactory))
        .build()
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

#[test]
fn sessions_can_be_created_independently() {
    let (orch, _transport) = test_orchestrator();

    let s1 = orch.session_create();
    let s2 = orch.session_create();
    assert_ne!(s1, s2);
}

#[test]
fn delete_session_rejects_push() {
    let (orch, transport) = test_orchestrator();

    let sid = orch.session_create();
    orch.session_delete(sid).unwrap();

    block_on(orch.push_user_message(user_msg(sid, "ghost")));
    assert!(transport.drain_sent().is_empty());
}

#[test]
fn push_without_session_id_is_rejected() {
    let (orch, transport) = test_orchestrator();

    block_on(orch.push_user_message(InboundMessage {
        message_id: "m1".into(),
        channel: "qq".into(),
        chat_id: "route-chat".into(),
        sender_id: None,
        session_id: String::new(),
        text: "via-route".into(),
    }));

    assert!(transport.drain_sent().is_empty());
}

#[test]
fn push_with_unknown_session_id_is_rejected() {
    let (orch, _) = test_orchestrator();

    block_on(orch.push_user_message(user_msg(SessionId(99), "orphan")));
}
