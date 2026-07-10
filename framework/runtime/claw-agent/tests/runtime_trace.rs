#![allow(clippy::unwrap_used)]

mod support;

use std::sync::{Arc, Mutex, MutexGuard};

use claw_log::{FlatTreeSubscriber, TraceSink};
use futures_lite::future::block_on;
use tracing::Level;

use support::{
    assistant_text, build_mem_system, drain_until_turn_ended, mem_root, serialize_script,
};

const USER_PAYLOAD_SECRET: &str = "user-payload-must-not-appear-in-trace-7f91";
const MODEL_PAYLOAD_SECRET: &str = "model-payload-must-not-appear-in-trace-8a42";

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
fn iteration_preparation_traces_auxiliary_llm_work_without_payloads() {
    let sink = RecordingSink::default();
    let subscriber = FlatTreeSubscriber::with_sink(sink.clone())
        .with_allowed_target_prefix("claw")
        .with_context_group_keys("run", ["session", "turn", "agent", "iteration"]);
    tracing::subscriber::set_global_default(subscriber)
        .expect("this single-test binary installs tracing exactly once");

    let _script = serialize_script();
    let root = mem_root("runtime-trace");
    // Leave ample scripted replies: auxiliary-call throttling is deliberately
    // not part of this trace contract. The same valid assistant reply works for
    // extraction, compaction, and the user-facing iteration.
    let replies = vec![assistant_text(MODEL_PAYLOAD_SECRET); 16];
    let system = build_mem_system(&root, replies);
    let session = system.new_session();
    let (control, mut events) = system.open_session(session).unwrap();

    // Two committed turns are needed for compaction: the first becomes the aged
    // prefix and the second alone exceeds the configured verbatim-tail budget.
    let oversized_input = USER_PAYLOAD_SECRET.repeat(1_024);
    for input in [oversized_input.clone(), oversized_input] {
        block_on(control.submit(input)).unwrap();
        let _ = drain_until_turn_ended(&mut events);
    }
    block_on(control.submit("trigger the next context preparation")).unwrap();
    let _ = drain_until_turn_ended(&mut events);

    // Join the worker before inspecting the complete trace so every selected
    // span also has its duration-closing exit record.
    drop(control);
    drop(events);
    drop(system);

    let lines = sink.lines();

    let extraction_chat = find_enter_with_field(&lines, "api.chat", "purpose", "memory_extraction");
    let extraction = assert_parent_named(&lines, extraction_chat, "context.extract");
    let extraction_prepare = assert_parent_named(&lines, extraction, "iteration.prepare");
    let extraction_agent = assert_parent_named(&lines, extraction_prepare, "agent");
    assert_iteration_sibling(&lines, extraction_prepare, extraction_agent);
    assert_render_child(&lines, extraction_prepare);
    assert_attempt_child(&lines, extraction_chat);

    let compaction_chat =
        find_enter_with_field(&lines, "api.chat", "purpose", "conversation_compaction");
    let compaction = assert_parent_named(&lines, compaction_chat, "context.compact");
    let compaction_prepare = assert_parent_named(&lines, compaction, "iteration.prepare");
    let compaction_agent = assert_parent_named(&lines, compaction_prepare, "agent");
    assert_iteration_sibling(&lines, compaction_prepare, compaction_agent);
    assert_render_child(&lines, compaction_prepare);
    assert_attempt_child(&lines, compaction_chat);

    for span in [
        extraction_chat,
        extraction,
        extraction_prepare,
        compaction_chat,
        compaction,
        compaction_prepare,
    ] {
        assert_has_exit(&lines, span);
    }

    let trace = lines.join("\n");
    for secret in [USER_PAYLOAD_SECRET, MODEL_PAYLOAD_SECRET] {
        assert!(
            !trace.contains(secret),
            "trace leaked payload marker {secret:?}: {trace}"
        );
    }
}

fn assert_iteration_sibling(lines: &[String], prepare: &str, agent: &str) {
    let agent_id = span_id(agent);
    assert_eq!(
        token(prepare, "parent"),
        Some(agent_id),
        "iteration.prepare must be a direct child of agent: {prepare}"
    );
    let iteration_loop = find_child(lines, agent_id, "iteration_loop");
    assert_eq!(
        token(iteration_loop, "iteration"),
        token(prepare, "iteration"),
        "prepare and iteration_loop siblings must identify the same iteration"
    );
    let iteration_chat = find_child_with_field(
        lines,
        span_id(iteration_loop),
        "api.chat",
        "purpose",
        "iteration",
    );
    assert_attempt_child(lines, iteration_chat);
    assert_has_exit(lines, iteration_chat);
    assert_has_exit(lines, iteration_loop);
}

fn assert_render_child(lines: &[String], prepare: &str) {
    let render = find_child(lines, span_id(prepare), "context.render");
    assert_has_exit(lines, render);
}

fn assert_attempt_child(lines: &[String], chat: &str) {
    assert!(
        token(chat, "max_attempts").is_some(),
        "api.chat must record its attempt budget: {chat}"
    );
    let attempt = find_child(lines, span_id(chat), "api.attempt");
    assert!(
        token(attempt, "attempt").is_some(),
        "api.attempt must record its attempt number: {attempt}"
    );
    assert!(
        token(attempt, "max_attempts").is_some(),
        "api.attempt must record its attempt budget: {attempt}"
    );
    assert_has_exit(lines, attempt);
}

fn assert_parent_named<'a>(lines: &'a [String], child: &str, expected: &str) -> &'a str {
    let parent_id = token(child, "parent")
        .filter(|parent| *parent != "none")
        .unwrap_or_else(|| panic!("span has no structural parent: {child}"));
    let parent = find_enter_by_id(lines, parent_id);
    assert_eq!(
        token(parent, "span-name"),
        Some(expected),
        "unexpected parent for {child}"
    );
    parent
}

fn find_enter_with_field<'a>(lines: &'a [String], name: &str, field: &str, value: &str) -> &'a str {
    lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "span-name") == Some(name)
                && token(line, field) == Some(value)
        })
        .map(String::as_str)
        .unwrap_or_else(|| panic!("missing {name} span with {field}={value}: {lines:#?}"))
}

fn find_child<'a>(lines: &'a [String], parent_id: &str, name: &str) -> &'a str {
    lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "parent") == Some(parent_id)
                && token(line, "span-name") == Some(name)
        })
        .map(String::as_str)
        .unwrap_or_else(|| panic!("missing {name} child of span {parent_id}: {lines:#?}"))
}

fn find_child_with_field<'a>(
    lines: &'a [String],
    parent_id: &str,
    name: &str,
    field: &str,
    value: &str,
) -> &'a str {
    lines
        .iter()
        .find(|line| {
            line_type(line) == Some("enter")
                && token(line, "parent") == Some(parent_id)
                && token(line, "span-name") == Some(name)
                && token(line, field) == Some(value)
        })
        .map(String::as_str)
        .unwrap_or_else(|| {
            panic!("missing {name} child of span {parent_id} with {field}={value}: {lines:#?}")
        })
}

fn find_enter_by_id<'a>(lines: &'a [String], id: &str) -> &'a str {
    lines
        .iter()
        .find(|line| line_type(line) == Some("enter") && token(line, "span") == Some(id))
        .map(String::as_str)
        .unwrap_or_else(|| panic!("missing enter record for span {id}: {lines:#?}"))
}

fn assert_has_exit(lines: &[String], enter: &str) {
    let id = span_id(enter);
    assert!(
        lines
            .iter()
            .any(|line| line_type(line) == Some("exit") && token(line, "span") == Some(id)),
        "missing exit record for span {id}: {enter}"
    );
}

fn span_id(line: &str) -> &str {
    token(line, "span").unwrap_or_else(|| panic!("line has no span id: {line}"))
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
