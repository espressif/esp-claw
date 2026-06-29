//! End-to-end tests for the public agent API.
//!
//! Every test drives the system ONLY through `claw_agent`'s public surface
//! ([`AgentSystem`] / [`Chat`]), with an in-memory filesystem and a scripted LLM
//! so they run hermetically (no network, no API key). The scripted HTTP double is
//! strict: it panics if the system makes more LLM calls than scripted, turning a
//! stray round into a hard failure instead of a silent false pass.

#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::sync::Arc;

use claw_agent::{AgentSystem, ClawApiConfig, PoolConfig, SharedTaskPool};
use claw_interface::{MemFs, SharedScriptHttp, StdThread};
use serde_json::json;

/// A test LLM config; the base URL is never dialed (HTTP is the scripted double).
fn test_llm_config() -> ClawApiConfig {
    ClawApiConfig {
        api_key: Some("sk-test".into()),
        backend_type: "openai_compatible".into(),
        model: Some("gpt-test".into()),
        base_url: Some("https://example.invalid".into()),
        supports_tools: true,
        ..Default::default()
    }
}

/// An OpenAI-compatible assistant turn returning plain text (hands control back).
fn plain_text(text: &str) -> String {
    json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
}

/// Build an in-memory agent system whose LLM serves `bodies` in order (strict).
///
/// The system mints each LLM client internally and chooses the transport by type
/// (`H = SharedScriptHttp`), so the script is installed into the thread-local
/// every minted client shares rather than injected as an instance.
fn system(bodies: Vec<String>) -> AgentSystem {
    SharedScriptHttp::install(bodies);
    let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread).expect("task pool"));
    AgentSystem::builder::<MemFs, SharedScriptHttp>()
        .llm(test_llm_config())
        .memory_dir("/mem/agents")
        .task_pool(pool)
        .build()
        .expect("build agent system")
}

#[test]
fn single_message_returns_the_agent_reply() {
    let system = system(vec![plain_text("hello there")]);
    let chat = system.chat();

    let replies = chat.send("hi");

    assert_eq!(replies, vec!["hello there".to_string()]);
}

#[test]
fn two_turns_reuse_the_same_chat_session() {
    let system = system(vec![plain_text("first reply"), plain_text("second reply")]);
    let chat = system.chat();

    assert_eq!(chat.send("one"), vec!["first reply".to_string()]);
    assert_eq!(chat.send("two"), vec!["second reply".to_string()]);
}

#[test]
fn separate_sessions_are_independent() {
    let system = system(vec![plain_text("answer-a"), plain_text("answer-b")]);
    let session_a = system.new_session();
    let session_b = system.new_session();

    assert_eq!(system.send(session_a, "x"), vec!["answer-a".to_string()]);
    assert_eq!(system.send(session_b, "y"), vec!["answer-b".to_string()]);
}
