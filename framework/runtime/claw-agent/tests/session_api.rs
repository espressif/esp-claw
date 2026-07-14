#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};

use claw_agent::{AgentError, AgentSystem, Message, OpenSessionError, SessionControlError};
use claw_agent::{SessionEvent, SessionEventStream, SessionId, StreamPart, TurnId};
use claw_interface::{
    BlockingHttpAdapter, Cancel, ClawHttp, HttpError, HttpJsonRequest, HttpResponseFuture,
    ImmediateTimer, MemFs, SharedScriptHttp, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use futures_lite::StreamExt;
use support::{
    assistant_text, build_mem_system, drain_until_turn_ended, install_script, llm_config, mem_root,
    persistence, serialize_script,
};

type SlowAgentSystem = AgentSystem<MemFs, Sse<SlowScriptHttp>, ImmediateTimer>;

#[derive(Default)]
struct SlowScriptHttp;

impl ClawHttp for SlowScriptHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            YieldTimes::new(16).await;
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            let mut inner = BlockingHttpAdapter::new(SharedScriptHttp::default());
            inner.post_json(request, cancel).await
        })
    }
}

struct YieldTimes {
    remaining: u32,
}

impl YieldTimes {
    const fn new(remaining: u32) -> Self {
        Self { remaining }
    }
}

impl Future for YieldTimes {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        if self.remaining == 0 {
            Poll::Ready(())
        } else {
            self.remaining -= 1;
            context.waker().wake_by_ref();
            Poll::Pending
        }
    }
}

#[test]
fn list_sessions_returns_session_ids() {
    let _script = serialize_script();
    let root = mem_root("agent-list-sessions");
    let system = build_mem_system(&root, Vec::new());
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);

    assert_eq!(system.list_sessions(), vec![session]);
}

#[test]
fn session_streams_root_reply_as_output() {
    let _script = serialize_script();
    let root = mem_root("agent-stream-output");
    let system = build_mem_system(&root, vec![assistant_text("hello there")]);
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(control.submit(Message::text("say hi"))).unwrap();
    let events = drain_until_turn_ended(&mut events);

    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted { turn: TurnId(1) })
    );
    assert_eq!(
        events.last(),
        Some(&SessionEvent::TurnEnded { turn: TurnId(1) })
    );
    let outputs: Vec<&str> = events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::Output(StreamPart::Delta(text)) => Some(text.as_str()),
            _ => None,
        })
        .collect();
    assert_eq!(outputs, vec!["hello there"]);
}

#[test]
fn open_unknown_session_returns_error() {
    let _script = serialize_script();
    let root = mem_root("agent-open-unknown");
    let system = build_mem_system(&root, Vec::new());

    assert!(matches!(
        system.open_session(SessionId(9)),
        Err(AgentError::OpenSession(OpenSessionError::SessionNotFound(
            SessionId(9)
        )))
    ));
}

#[test]
fn second_submit_returns_busy_until_current_turn_ends() {
    let _script = serialize_script();
    let root = mem_root("agent-submit-busy");
    let system = build_slow_system(
        &root,
        vec![assistant_text("first"), assistant_text("second")],
    );
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(async {
        control.submit(Message::text("first")).await.unwrap();
        assert!(matches!(
            control.submit(Message::text("second")).await,
            Err(SessionControlError::Busy(busy_session)) if busy_session == session
        ));
    });
    let first_events = drain_until_turn_ended(&mut events);

    assert!(first_events.iter().any(
        |event| matches!(event, SessionEvent::Output(StreamPart::Delta(text)) if text == "first")
    ));
    block_on(control.submit(Message::text("second"))).unwrap();
    let second_events = drain_until_turn_ended(&mut events);
    assert!(second_events.iter().any(
        |event| matches!(event, SessionEvent::Output(StreamPart::Delta(text)) if text == "second")
    ));
}

#[test]
fn session_control_methods_are_idempotent() {
    let _script = serialize_script();
    let root = mem_root("agent-control-idempotent");
    let system = build_slow_system(&root, vec![assistant_text("cancelled")]);
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(async {
        control.submit(Message::text("cancel me")).await.unwrap();
        control.interrupt().await.unwrap();
        control.interrupt().await.unwrap();
        control.cancel().await.unwrap();
        control.cancel().await.unwrap();
    });

    let events = drain_until_turn_ended(&mut events);
    assert!(matches!(
        events.first(),
        Some(SessionEvent::TurnStarted { turn: TurnId(1) })
    ));
    assert_eq!(
        events.last(),
        Some(&SessionEvent::TurnEnded { turn: TurnId(1) })
    );
}

#[test]
fn close_session_cancels_active_work_and_closes_events() {
    let _script = serialize_script();
    let root = mem_root("agent-close-session");
    let system = build_slow_system(&root, vec![assistant_text("should not surface")]);
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(async {
        control.submit(Message::text("close me")).await.unwrap();
        control.close_session().await.unwrap();
    });
    let events = drain_until_closed(&mut events);

    assert_eq!(events.last(), Some(&SessionEvent::Closed));
    assert!(
        !events
            .iter()
            .any(|event| matches!(event, SessionEvent::Output(StreamPart::Delta(_)))),
        "closed stream should be cancelled without output: {events:?}"
    );
    assert!(system.list_sessions().contains(&session));
    assert!(
        block_on(control.submit(Message::text("after close"))).is_err(),
        "closed control should reject new submits"
    );
}

#[test]
fn delete_session_removes_session_and_closes_open_stream() {
    let _script = serialize_script();
    let root = mem_root("agent-delete-session");
    let system = build_mem_system(&root, Vec::new());
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (_control, mut events) = system.open_session(session).unwrap();

    system.delete_session(session).unwrap();
    let events = drain_until_closed(&mut events);

    assert_eq!(events.last(), Some(&SessionEvent::Closed));
    assert!(!system.list_sessions().contains(&session));
    assert!(matches!(
        system.open_session(session),
        Err(AgentError::OpenSession(OpenSessionError::SessionNotFound(
            missing
        ))) if missing == session
    ));
}

fn build_slow_system(root: &str, bodies: Vec<String>) -> SlowAgentSystem {
    install_script(bodies);
    let system = SlowAgentSystem::new::<StdThread, TokioExecutor>(persistence(root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    system
}

fn drain_until_closed(events: &mut SessionEventStream) -> Vec<SessionEvent> {
    block_on(async move {
        let mut collected = Vec::new();
        while let Some(event) = events.next().await {
            let closed = event == SessionEvent::Closed;
            collected.push(event);
            if closed {
                break;
            }
        }
        collected
    })
}
