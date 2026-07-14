#![allow(clippy::unwrap_used)]

mod support;

use std::sync::{Arc, Mutex, MutexGuard};

use claw_agent::{AgentPersistenceConfig, AgentSystem, Message};
use claw_interface::{
    BlockingHttpAdapter, DiskFs, ImmediateTimer, SharedScriptHttp, StdThread, TokioExecutor,
};
use claw_log::{FlatTreeSubscriber, TraceSink};
use futures_lite::future::block_on;
use tempdir::TempDir;
use tracing::Level;

use support::{
    assistant_text, drain_until_turn_ended, install_script, llm_config, serialize_script, Sse,
};

type TraceAgentSystem =
    AgentSystem<DiskFs, Sse<BlockingHttpAdapter<SharedScriptHttp>>, ImmediateTimer>;

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
        .with_context_group_keys("run", ["system", "session", "turn", "agent", "iteration"]);
    tracing::subscriber::set_global_default(subscriber)
        .expect("this single-test binary installs tracing exactly once");

    let _script = serialize_script();
    let root = TempDir::new("logical-task-trace").unwrap();
    let root = root.path().to_string_lossy().into_owned();
    let first_system = build_trace_system(&root, vec![assistant_text("done"); 8]);
    let restored_session = first_system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = first_system.open_session(restored_session).unwrap();

    block_on(control.submit(Message::text("trace one agent turn"))).unwrap();
    let _ = drain_until_turn_ended(&mut events);
    block_on(control.close_session()).unwrap();

    drop(control);
    drop(events);
    drop(first_system);

    let second_system = build_trace_system(&root, Vec::new());
    let created_session = second_system.new_session(claw_agent::SessionPersistence::Ephemeral);
    let (restored_control, restored_events) = second_system.open_session(restored_session).unwrap();
    drop(restored_control);
    drop(restored_events);
    drop(second_system);

    let lines = sink.lines();
    let orchestrators = lines
        .iter()
        .filter(|line| {
            line_type(line) == Some("enter") && token(line, "span-name") == Some("orchestrator")
        })
        .collect::<Vec<_>>();
    assert_eq!(orchestrators.len(), 2, "{}", lines.join("\n"));
    let first_system_id = token(orchestrators[0], "system").expect("first system context id");
    let second_system_id = token(orchestrators[1], "system").expect("second system context id");
    assert_ne!(first_system_id, second_system_id);
    for orchestrator in &orchestrators {
        let orchestrator_span = token(orchestrator, "span").expect("orchestrator span id");
        let factory = lines
            .iter()
            .find(|line| {
                line_type(line) == Some("enter")
                    && token(line, "span-name") == Some("agent.factory")
                    && token(line, "parent") == Some(orchestrator_span)
            })
            .expect("agent factory is system-scoped startup");
        let factory_span = token(factory, "span").expect("agent factory span id");
        assert!(lines.iter().any(|line| {
            line_type(line) == Some("enter")
                && token(line, "span-name") == Some("skill.catalog")
                && token(line, "parent") == Some(factory_span)
        }));
    }
    let expected_orchestrator_task = format!("orchestrator-{first_system_id}");
    assert_eq!(
        token(orchestrators[0], "task"),
        Some(expected_orchestrator_task.as_str())
    );

    let restored_session_wire = restored_session.to_wire();
    let restore = lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "span-name") == Some("session.restore")
                && token(line, "session") == Some(restored_session_wire.as_str())
        })
        .expect("session restore enter line");
    assert_eq!(
        token(restore, "parent"),
        token(orchestrators[1], "span"),
        "{restore}"
    );
    let restore_span = token(restore, "span").expect("session restore span id");
    assert!(lines.iter().any(|line| {
        line_type(line) == Some("enter")
            && token(line, "span-name") == Some("agent.create")
            && token(line, "parent") == Some(restore_span)
    }));

    let created_session_wire = created_session.to_wire();
    let session_create = lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "span-name") == Some("session.create")
                && token(line, "session") == Some(created_session_wire.as_str())
        })
        .expect("session create enter line");
    assert_eq!(token(session_create, "system"), Some(second_system_id));

    let session_actor = lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "span-name") == Some("session")
                && token(line, "target") == Some("claw_core::orchestrator::engine")
                && token(line, "session") == Some(restored_session_wire.as_str())
        })
        .expect("session actor enter line");
    let session_id = token(session_actor, "session").expect("session context id");
    assert_eq!(
        token(session_actor, "task"),
        Some(session_id),
        "{session_actor}"
    );
    assert!(token(session_actor, "system").is_some(), "{session_actor}");
    assert!(!session_actor.contains("trace.task="), "{session_actor}");

    let agent = lines
        .iter()
        .find(|line| line_type(line) == Some("enter") && token(line, "span-name") == Some("agent"))
        .expect("agent enter line");
    let agent_id = token(agent, "agent").expect("agent context id");

    assert_eq!(token(agent, "task"), Some(agent_id), "{agent}");
    assert!(token(agent, "system").is_some(), "{agent}");
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

fn build_trace_system(root: &str, bodies: Vec<String>) -> TraceAgentSystem {
    install_script(bodies);
    let system = TraceAgentSystem::new::<StdThread, TokioExecutor>(AgentPersistenceConfig {
        persistence_root: root.to_string(),
        skill_roots: Vec::new(),
    })
    .unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    system
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
