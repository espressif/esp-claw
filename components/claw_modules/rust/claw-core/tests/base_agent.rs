//! Integration tests for [`claw_core::agent::BaseAgent`].
//!
//! Scripted tests drive the state machine with canned HTTP responses.
//! Memory tests use a real `DiskFs` writing to `output/<test_name>/` under
//! the crate root so that transcript files can be inspected after a run.
//! The live test talks to a real LLM and verifies multi-turn memory continuity.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use claw_api::{ClawApi, ClawApiConfig};
use claw_core::agent::{AgentId, BaseAgent, CancelReason, TickOutcome};
use claw_core::{Tool, ToolGroup, ToolSet};
use claw_interface::RealHttp;
use serde_json::{json, Value};

mod common;
use common::{
    agent_builder, capturing_llm, failing_llm, run_to_completion,
    scripted_llm as common_scripted_llm, test_memory, test_output_dir, test_pool, EchoTool,
};

// ---------------------------------------------------------------------------
// Helpers: env / live API
// ---------------------------------------------------------------------------

fn load_local_env() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join(".env.local");
    if path.is_file() {
        dotenvy::from_path(&path)
            .unwrap_or_else(|e| panic!("failed to load {}: {e}", path.display()));
    }
}

fn local_env_path() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join(".env.local")
}

fn require_local_api_key() -> String {
    let env_path = local_env_path();
    if !env_path.is_file() {
        panic!(
            "live test requires {} — copy .env.example to .env.local and set CLAW_LLM_API_KEY",
            env_path.display()
        );
    }
    load_local_env();
    std::env::var("CLAW_LLM_API_KEY")
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| {
            panic!(
                "live test requires CLAW_LLM_API_KEY in {} (file exists but key is missing or empty)",
                env_path.display()
            )
        })
}

fn local_base_url() -> String {
    load_local_env();
    std::env::var("CLAW_LLM_BASE_URL").expect("CLAW_LLM_BASE_URL must be set in .env.local")
}

fn local_model() -> String {
    load_local_env();
    std::env::var("CLAW_LLM_MODEL").expect("CLAW_LLM_MODEL must be set in .env.local")
}

// ---------------------------------------------------------------------------
// Helpers: HTTP stubs
// ---------------------------------------------------------------------------
//
// Body constants and the strict HTTP doubles / LLM builders / agent + memory
// helpers (`scripted_llm`, `capturing_llm`, `failing_llm`, `agent_builder`,
// `test_memory`, `run_to_completion`, `EchoTool`, …) are shared from `common`.
// The live transport is `RealHttp` (the `realhttp` feature).

const PLAIN_TEXT_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","content":"pong"}}]}"#;

const TOOL_CALL_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"t1","function":{"name":"echo","arguments":"{\"input\":\"hello\"}"}}]}}]}"#;

const END_CONVERSATION_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"e1","function":{"name":"end_conversation","arguments":"{\"final_message\":\"all done\"}"}}]}}]}"#;

/// Tool-capable strict scripted LLM over `&str` bodies (thin wrapper over the
/// shared [`common::scripted_llm`], which takes owned `String`s).
fn scripted_llm(bodies: Vec<&str>) -> ClawApi<claw_interface::ScriptedHttp> {
    common_scripted_llm(bodies.into_iter().map(String::from).collect())
}

fn live_llm() -> ClawApi<RealHttp> {
    let api_key = require_local_api_key();
    ClawApi::init(
        ClawApiConfig {
            api_key: Some(api_key),
            backend_type: "openai_compatible".into(),
            model: Some(local_model()),
            base_url: Some(local_base_url()),
            supports_tools: false,
            timeout_ms: 60_000,
            ..Default::default()
        },
        RealHttp::new(),
    )
    .expect("live llm")
}

// ---------------------------------------------------------------------------
// Tests: AgentId
// ---------------------------------------------------------------------------

#[test]
fn agent_id_wire_format() {
    let id = AgentId(7);
    assert_eq!(id.to_string(), "agent-7");

    let parsed: AgentId = "agent-7".parse().expect("parse");
    assert_eq!(parsed, id);
}

#[test]
fn agent_id_serializes_to_prefixed_string() {
    let value = serde_json::to_value(AgentId(3)).expect("serialize");
    assert_eq!(value, json!("agent-3"));
}

// ---------------------------------------------------------------------------
// Tests: BaseAgent state machine
// ---------------------------------------------------------------------------

#[test]
fn tick_returns_idle_before_run() {
    let dir = test_output_dir("tick_returns_idle_before_run");
    let mut agent = agent_builder(scripted_llm(vec![]), AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    assert!(!agent.is_running());
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

#[test]
fn cancel_reports_cancelled_and_goes_idle() {
    let dir = test_output_dir("cancel_reports_cancelled_and_goes_idle");
    let mut agent = agent_builder(scripted_llm(vec![]), AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    agent.run("do something");
    agent
        .cancel(CancelReason::UserRequested)
        .expect("cancel accepted while a task is queued");

    // The first tick drains AppendMessage then Cancel, never calling the LLM (so
    // the empty script is fine).
    assert!(matches!(
        agent.tick(),
        TickOutcome::Cancelled {
            reason: CancelReason::UserRequested
        }
    ));
    assert!(!agent.is_running());
}

#[test]
fn cancel_records_interruption_marker_in_memory() {
    let dir = test_output_dir("cancel_records_interruption_marker_in_memory");
    let memory = test_memory(AgentId(1), dir.display().to_string(), test_pool());
    let view = memory.clone();
    let mut agent = BaseAgent::builder(scripted_llm(vec![]), memory)
        .build()
        .expect("build");

    agent.run("do something risky");
    agent
        .cancel(CancelReason::UserRequested)
        .expect("cancel accepted while a task is queued");
    assert!(matches!(agent.tick(), TickOutcome::Cancelled { .. }));

    let messages = view.messages();
    let items = messages.as_array().expect("messages is an array");
    let contents: Vec<&str> = items
        .iter()
        .filter_map(|m| m.get("content").and_then(Value::as_str))
        .collect();

    // The abandoned user turn is retained (no memory loss)...
    assert!(
        contents.contains(&"do something risky"),
        "abandoned user turn missing: {contents:?}"
    );
    // ...and the disruption is explained by an interruption marker.
    assert!(
        contents
            .iter()
            .any(|c| c.contains("interrupted") && c.contains("user")),
        "interruption marker missing: {contents:?}"
    );
}

#[test]
fn single_turn_yields_answer() {
    let dir = test_output_dir("single_turn_yields_answer");
    let mut agent = agent_builder(
        scripted_llm(vec![PLAIN_TEXT_BODY]),
        AgentId(1),
        dir.display().to_string(),
    )
    .with_system_prompt("You are a test assistant.")
    .build()
    .expect("build");

    agent.run("say pong");
    let text = run_to_completion(&mut agent);
    // A plain-text answer is non-terminal: the agent goes idle, not done.
    assert!(!agent.is_running());
    assert_eq!(text, "pong");
}

#[test]
fn tool_round_works_then_yields() {
    let dir = test_output_dir("tool_round_works_then_yields");
    // First LLM call returns a tool call; second returns plain text.
    let llm = scripted_llm(vec![TOOL_CALL_BODY, PLAIN_TEXT_BODY]);
    let tools = ToolSet::from_groups([ToolGroup::new("echo_group", [Tool::new(EchoTool)])])
        .expect("build tool set");
    let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
        .with_tools(tools)
        .build()
        .expect("build");

    agent.run("use the echo tool then answer");

    // First tick: append + tool round → Working (tool detail stays internal).
    assert!(matches!(agent.tick(), TickOutcome::Working));
    assert!(agent.is_running());

    // Second tick: plain text → Yielded → idle.
    assert!(matches!(agent.tick(), TickOutcome::Yielded { .. }));
    assert!(!agent.is_running());
}

#[test]
fn end_conversation_tool_ends_task() {
    let dir = test_output_dir("end_conversation_tool_ends_task");
    let mut agent = agent_builder(
        scripted_llm(vec![END_CONVERSATION_BODY]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("finish up");

    // The internal end_conversation tool runs and the signal it raises ends the
    // task in the same tick.
    assert!(matches!(
        agent.tick(),
        TickOutcome::Ended { final_message } if final_message == "all done"
    ));
    assert!(!agent.is_running());
}

#[test]
fn append_after_end_starts_fresh_task() {
    let dir = test_output_dir("append_after_end_starts_fresh_task");
    let llm = scripted_llm(vec![END_CONVERSATION_BODY, PLAIN_TEXT_BODY]);
    let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    agent.run("first");
    assert!(agent.tick().is_terminal());

    // Re-task the same (now idle) agent: AppendMessage starts a fresh task.
    agent.run("second");
    let text = run_to_completion(&mut agent);
    assert_eq!(text, "pong");
}

#[test]
fn failed_http_reports_failed_and_goes_idle() {
    let dir = test_output_dir("failed_http_reports_failed_and_goes_idle");
    let llm = failing_llm();
    let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    agent.run("hello");

    assert!(matches!(agent.tick(), TickOutcome::Failed(_)));
    assert!(!agent.is_running());
}

#[test]
fn abort_before_iteration_reruns_next_tick() {
    let dir = test_output_dir("abort_before_iteration_reruns_next_tick");
    let mut agent = agent_builder(
        scripted_llm(vec![PLAIN_TEXT_BODY]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("hello");
    agent.abort_handle().abort();

    // Preempted before the LLM call: stays running and re-iterates next tick.
    assert!(matches!(agent.tick(), TickOutcome::Working));
    assert!(agent.is_running());

    // The flag was consumed; the next tick reaches the LLM and yields.
    assert!(matches!(agent.tick(), TickOutcome::Yielded { .. }));
    assert!(!agent.is_running());
}

#[test]
fn tick_stays_idle_after_yield() {
    let dir = test_output_dir("tick_stays_idle_after_yield");
    let mut agent = agent_builder(
        scripted_llm(vec![PLAIN_TEXT_BODY]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("hi");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { .. }));

    // No further work and no new input: subsequent ticks stay idle (no LLM call).
    assert!(matches!(agent.tick(), TickOutcome::Idle));
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

// ---------------------------------------------------------------------------
// Tests: memory persistence (DiskFs)
// ---------------------------------------------------------------------------

#[test]
fn memory_written_to_disk_after_turn() {
    let dir = test_output_dir("memory_written_to_disk_after_turn");
    let data_file = dir.join("conversation-1.jsonl");

    let mut agent = agent_builder(
        scripted_llm(vec![PLAIN_TEXT_BODY]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("write something to disk");
    run_to_completion(&mut agent);

    // The conversation data file must exist and contain JSONL records.
    let content = std::fs::read_to_string(&data_file)
        .unwrap_or_else(|_| panic!("conversation data file not found: {}", data_file.display()));
    assert!(!content.is_empty(), "data file is empty");
    // Each non-empty line must parse as a JSON object.
    for line in content.lines().filter(|l| !l.is_empty()) {
        serde_json::from_str::<Value>(line)
            .unwrap_or_else(|err| panic!("data file has invalid JSON line: {err}\nLine: {line}"));
    }
}

#[test]
fn second_turn_includes_first_turn_context() {
    let dir = test_output_dir("second_turn_includes_first_turn_context");

    // Turn 1: complete a turn so the user + assistant messages are committed.
    {
        let (llm, _http) = capturing_llm(vec![PLAIN_TEXT_BODY.into()]);
        let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
            .build()
            .expect("build");
        agent.run("my secret word is BANANA");
        run_to_completion(&mut agent);
    }

    // Turn 2: new agent instance, same agent_id and dir → loads persisted transcript.
    let (llm, http2) = capturing_llm(vec![PLAIN_TEXT_BODY.into()]);
    {
        let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
            .build()
            .expect("build");
        agent.run("what was my secret word?");
        run_to_completion(&mut agent);
    }

    // The second LLM call must have received the first turn's messages in context.
    let bodies = http2.captured_bodies();
    assert!(!bodies.is_empty(), "no LLM calls captured for turn 2");
    let second_call_body = bodies[0].to_string();
    assert!(
        second_call_body.contains("BANANA"),
        "turn 1 context (BANANA) missing from turn 2 LLM request:\n{second_call_body}"
    );
}

// ---------------------------------------------------------------------------
// Live test: multi-turn chat with real LLM
// ---------------------------------------------------------------------------

#[test]
fn live_two_turn_chat_uses_memory() {
    let dir = test_output_dir("live_two_turn_chat_uses_memory");
    let pool = test_pool();

    // Turn 1 — plant a memorable fact.
    {
        let mut agent = BaseAgent::builder(
            live_llm(),
            test_memory(AgentId(1), dir.display().to_string(), Arc::clone(&pool)),
        )
        .with_system_prompt("You are a test assistant. Be brief and exact.")
        .build()
        .expect("build");
        agent.run("Remember this code word: FLAMINGO. Reply with exactly: acknowledged");

        let text = run_to_completion(&mut agent);
        assert!(
            text.to_lowercase().contains("acknowledged"),
            "turn 1 unexpected: {text}"
        );
    }

    // Turn 2 — new agent instance loads the persisted transcript and recalls the fact.
    {
        let mut agent = BaseAgent::builder(
            live_llm(),
            test_memory(AgentId(1), dir.display().to_string(), pool),
        )
        .with_system_prompt("You are a test assistant. Be brief and exact.")
        .build()
        .expect("build");
        agent.run("What was the code word I gave you?");

        let text = run_to_completion(&mut agent);
        assert!(
            text.to_uppercase().contains("FLAMINGO"),
            "turn 2 did not recall FLAMINGO: {text}"
        );
    }
}
