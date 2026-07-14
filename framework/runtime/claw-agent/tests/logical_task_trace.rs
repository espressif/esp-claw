#![allow(clippy::unwrap_used)]

mod support;

use std::sync::{Arc, Mutex, MutexGuard};

use claw_agent::Message;
use claw_log::{FlatTreeSubscriber, TraceSink};
use futures_lite::future::block_on;
use tracing::Level;

use support::{
    assistant_text, build_mem_system, drain_until_turn_ended, mem_root, serialize_script,
};

#[derive(Clone, Default)]
struct RecordingSink(Arc<Mutex<Vec<String>>>);

impl RecordingSink {
    fn lines(&self) -> Vec<String> {
        lock(&self.0).clone()
    }
}

impl TraceSink for RecordingSink {
    fn write_line(&self, _level: Level, _tag: &str, line: &str) {
        lock(&self.0).push(line.to_string());
    }
}

#[test]
fn async_runtime_roots_use_logical_task_lanes_with_full_context() {
    let sink = RecordingSink::default();
    let subscriber = FlatTreeSubscriber::with_sink(sink.clone())
        .with_allowed_target_prefix("claw")
        .with_context_group_keys("run", ["session", "turn", "agent", "iteration"]);
    tracing::subscriber::set_global_default(subscriber)
        .expect("this single-test binary installs tracing exactly once");

    let _script = serialize_script();
    let root = mem_root("logical-task-trace");
    let system = build_mem_system(&root, vec![assistant_text("done"); 8]);
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(control.submit(Message::text("trace one agent turn"))).unwrap();
    let _ = drain_until_turn_ended(&mut events);

    drop(control);
    drop(events);
    drop(system);

    let lines = sink.lines();
    let session_actor = lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "span-name") == Some("session")
                && token(line, "target") == Some("claw_core::orchestrator::engine")
        })
        .expect("session actor enter line");
    let session_id = token(session_actor, "session").expect("session context id");
    assert_eq!(
        token(session_actor, "task"),
        Some(session_id),
        "{session_actor}"
    );
    assert!(!session_actor.contains("trace.task="), "{session_actor}");

    let agent = lines
        .iter()
        .find(|line| line_type(line) == Some("enter") && token(line, "span-name") == Some("agent"))
        .expect("agent enter line");
    let agent_id = token(agent, "agent").expect("agent context id");

    assert_eq!(token(agent, "task"), Some(agent_id), "{agent}");
    assert!(token(agent, "session").is_some(), "{agent}");
    assert!(token(agent, "turn").is_some(), "{agent}");
    assert!(!agent.contains("trace.task="), "{agent}");
}

fn token<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split(' ').find_map(|raw| {
        let token = raw.trim_matches(|ch| ch == '<' || ch == '>');
        token.strip_prefix(key)?.strip_prefix('=')
    })
}

fn line_type(line: &str) -> Option<&str> {
    line.split(' ').nth(2)
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
