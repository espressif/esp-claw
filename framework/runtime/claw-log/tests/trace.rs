#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::sync::{Arc, Mutex};

use claw_log::{FlatTreeSubscriber, TraceSink};
use tracing::Level;

#[derive(Clone, Default)]
struct VecSink(Arc<Mutex<Vec<(Level, String, String)>>>);

impl VecSink {
    fn lines(&self) -> Vec<String> {
        self.0
            .lock()
            .unwrap()
            .iter()
            .map(|(_, _, line)| line.clone())
            .collect()
    }
}

impl TraceSink for VecSink {
    fn write_line(&self, level: Level, tag: &str, line: &str) {
        self.0
            .lock()
            .unwrap()
            .push((level, tag.to_string(), line.to_string()));
    }
}

#[test]
fn event_message_newlines_are_collapsed() {
    let sink = VecSink::default();
    tracing::subscriber::with_default(FlatTreeSubscriber::with_sink(sink.clone()), || {
        tracing::info!("a\nb\rc");
    });

    let event = event_line(&sink);
    assert!(event.ends_with("a b c"), "{event}");
}

#[test]
fn event_name_whitespace_is_tokenized() {
    let sink = VecSink::default();
    tracing::subscriber::with_default(FlatTreeSubscriber::with_sink(sink.clone()), || {
        tracing::info!(name: "event src/x.rs:10", "named");
    });

    let event = event_line(&sink);
    assert_eq!(token(&event, "event-name"), Some("event_src/x.rs:10"));
}

#[test]
fn context_group_fields_are_emitted_as_context_and_other_fields_stay_custom() {
    let sink = VecSink::default();
    tracing::subscriber::with_default(run_subscriber(sink.clone()), || {
        tracing::info_span!(
            "session",
            run.session = "s-1",
            a = 1u64,
            http.method = "GET"
        )
        .in_scope(|| {});
    });

    let enter = sink
        .lines()
        .into_iter()
        .find(|line| line_type(line) == Some("enter"))
        .expect("enter line");
    assert_eq!(token(&enter, "context"), Some("run"));
    assert_eq!(token(&enter, "session"), Some("s-1"));
    assert_eq!(token(&enter, "a"), Some("1"));
    assert!(enter.contains("http.method=GET"), "{enter}");
}

#[test]
fn enter_line_carries_timestamp_and_type() {
    let sink = VecSink::default();
    tracing::subscriber::with_default(run_subscriber(sink.clone()), || {
        tracing::info_span!("session", run.session = "s-1").in_scope(|| {});
    });

    let enter = sink
        .lines()
        .into_iter()
        .find(|line| line_type(line) == Some("enter"))
        .expect("enter line");
    assert!(enter.starts_with("TRACE "));
    let timestamp = enter.split(' ').nth(1).expect("timestamp token");
    assert!(timestamp.parse::<u64>().is_ok(), "ts not numeric: {enter}");
}

#[test]
fn nested_spans_and_event_carry_ids_parent_edges_and_grouped_context() {
    let sink = VecSink::default();

    tracing::subscriber::with_default(run_subscriber(sink.clone()), || {
        let session = tracing::info_span!("session", run.session = "s-1");
        let _session = session.enter();
        tracing::info!(name: "thinking", "root thinking");
        {
            let agent = tracing::info_span!("agent", run.agent = "a-2", depth = 1u64);
            let _agent = agent.enter();
            tracing::info!(name: "tool_call", tool = "files", "calling tool");
        }
    });

    let lines = sink.lines();

    let session_enter = lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter") && token(line, "span-name") == Some("session")
        })
        .expect("session enter line");
    assert_eq!(token(session_enter, "parent"), Some("none"));
    assert_eq!(token(session_enter, "context"), Some("run"));
    assert_eq!(token(session_enter, "session"), Some("s-1"));
    let session_id = token(session_enter, "span").expect("session span");

    let agent_enter = lines
        .iter()
        .find(|line| line_type(line) == Some("enter") && token(line, "span-name") == Some("agent"))
        .expect("agent enter line");
    assert_eq!(token(agent_enter, "parent"), Some(session_id));
    let agent_id = token(agent_enter, "span").expect("agent span");
    assert_eq!(token(agent_enter, "agent"), Some("a-2"));
    assert_eq!(token(agent_enter, "session"), None);
    assert_eq!(token(agent_enter, "depth"), Some("1"));

    let root_event = lines
        .iter()
        .find(|line| line_type(line) == Some("event") && line.ends_with("root thinking"))
        .expect("root event");
    assert_eq!(token(root_event, "span"), Some(session_id));
    assert_eq!(token(root_event, "event-name"), Some("thinking"));

    let tool_event = lines
        .iter()
        .find(|line| line_type(line) == Some("event") && line.contains("calling tool"))
        .expect("tool event");
    assert_eq!(token(tool_event, "span"), Some(agent_id));
    assert_eq!(token(tool_event, "event-name"), Some("tool_call"));
    assert!(tool_event.contains("tool=files"));
    assert_eq!(token(tool_event, "context"), None);
    assert_eq!(token(tool_event, "session"), None);

    assert!(lines
        .iter()
        .any(|line| line_type(line) == Some("exit") && token(line, "span") == Some(agent_id)));
    assert!(lines
        .iter()
        .any(|line| line_type(line) == Some("exit") && token(line, "span") == Some(session_id)));
}

#[test]
fn target_allowlist_drops_foreign_targets() {
    let sink = VecSink::default();
    let subscriber = FlatTreeSubscriber::with_sink(sink.clone()).with_allowed_target_prefix("claw");

    tracing::subscriber::with_default(subscriber, || {
        tracing::info!(target: "reqwest::connect", "pool checkout");
        tracing::info!(target: "claw_core::demo", "kept");
    });

    let lines = sink.lines();
    assert!(
        lines.iter().any(|line| line.ends_with("kept")),
        "claw-targeted event should be traced"
    );
    assert!(
        !lines.iter().any(|line| line.contains("pool checkout")),
        "foreign-targeted event should be filtered out"
    );
}

#[test]
fn event_outside_any_span_reports_span_none() {
    let sink = VecSink::default();
    tracing::subscriber::with_default(FlatTreeSubscriber::with_sink(sink.clone()), || {
        tracing::info!("no span here");
    });

    let event = event_line(&sink);
    assert_eq!(token(&event, "span"), Some("none"));
}

#[test]
fn logical_tasks_separate_overlapping_async_span_lifetimes() {
    let sink = VecSink::default();

    tracing::subscriber::with_default(run_subscriber(sink.clone()), || {
        let session = tracing::info_span!("session", run.session = "s-1");
        let _session = session.enter();
        let turn = tracing::info_span!("turn", run.turn = "t-1");
        let _turn = turn.enter();

        // These sibling spans model separately instrumented async agent futures:
        // both are alive at once on one executor thread and may close in an
        // order unrelated to their creation order.
        let agent_2 = tracing::info_span!(
            "agent",
            trace.task = "agent-2",
            run.agent = "agent-2",
            depth = 1u64,
        );
        let agent_3 = tracing::info_span!(
            "agent",
            trace.task = "agent-3",
            run.agent = "agent-3",
            depth = 1u64,
        );

        agent_2.in_scope(|| {
            let iteration = tracing::info_span!("iteration_loop", run.iteration = "iteration-2");
            iteration.in_scope(|| tracing::info!(name: "polled", owner = "agent-2"));
        });
        agent_3.in_scope(|| {
            let iteration = tracing::info_span!("iteration_loop", run.iteration = "iteration-3");
            iteration.in_scope(|| tracing::info!(name: "polled", owner = "agent-3"));
        });

        std::thread::Builder::new()
            .name("other-executor".to_string())
            .spawn(move || drop(agent_2))
            .expect("spawn close thread")
            .join()
            .expect("close thread");
        drop(agent_3);
    });

    let lines = sink.lines();
    for agent in ["agent-2", "agent-3"] {
        let enter = lines
            .iter()
            .find(|line| {
                line_type(line) == Some("enter")
                    && token(line, "span-name") == Some("agent")
                    && token(line, "agent") == Some(agent)
            })
            .expect("logical agent enter");
        let span_id = token(enter, "span").expect("agent span id");

        assert_eq!(token(enter, "task"), Some(agent), "{enter}");
        assert_eq!(token(enter, "session"), Some("s-1"), "{enter}");
        assert_eq!(token(enter, "turn"), Some("t-1"), "{enter}");
        assert!(!enter.contains("trace.task="), "{enter}");

        let iteration = lines
            .iter()
            .find(|line| {
                line_type(line) == Some("enter")
                    && token(line, "iteration")
                        == Some(if agent == "agent-2" {
                            "iteration-2"
                        } else {
                            "iteration-3"
                        })
            })
            .expect("iteration enter");
        assert_eq!(token(iteration, "task"), Some(agent), "{iteration}");

        let event = lines
            .iter()
            .find(|line| line_type(line) == Some("event") && token(line, "owner") == Some(agent))
            .expect("agent event");
        assert_eq!(token(event, "task"), Some(agent), "{event}");

        let exit = lines
            .iter()
            .find(|line| line_type(line) == Some("exit") && token(line, "span") == Some(span_id))
            .expect("agent exit");
        assert_eq!(token(exit, "task"), Some(agent), "{exit}");
    }
}

fn event_line(sink: &VecSink) -> String {
    sink.lines()
        .into_iter()
        .find(|line| line_type(line) == Some("event"))
        .expect("event line")
}

fn token<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split(' ').find_map(|raw| {
        let tok = raw.trim_matches(|c| c == '<' || c == '>');
        tok.strip_prefix(key)?.strip_prefix('=')
    })
}

fn line_type(line: &str) -> Option<&str> {
    line.split(' ').nth(2)
}

fn run_subscriber(sink: VecSink) -> FlatTreeSubscriber<VecSink> {
    FlatTreeSubscriber::with_sink(sink)
        .with_context_group_keys("run", ["session", "turn", "agent", "iteration"])
}
