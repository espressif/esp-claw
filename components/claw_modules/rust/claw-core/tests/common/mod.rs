//! Shared test harness for the `BaseAgent` integration tests.
//!
//! Every helper here favours *strictness* so tests fail loudly on the unexpected:
//! the HTTP doubles panic when the agent calls the LLM more often than scripted,
//! which turns a stray (or missing) LLM round into a hard failure instead of a
//! silent false pass.

#![allow(dead_code)]
#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::path::{Path, PathBuf};
use std::sync::Arc;

use claw_api::{ClawApi, ClawApiConfig};
use claw_core::agent::{AgentId, BaseAgent, BaseAgentBuilder, TickOutcome};
use claw_core::{ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};
use claw_interface::{
    CapturingHttp, ClawHttp, DiskFs, FailingHttp, NeverHttp, ScriptStep, ScriptedHttp, StdThread,
};
use claw_memory::{TranscriptConfig, TranscriptStore};
use claw_utils::{PoolConfig, SharedTaskPool};
use serde_json::{json, Value};

// ===========================================================================
// Canned LLM response bodies (OpenAI-compatible `choices` shape)
// ===========================================================================

/// An assistant turn that returns plain text and hands control back.
pub fn body_plain_text(text: &str) -> String {
    json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
}

/// An assistant turn that issues a single tool call. `arguments_json` is the raw
/// JSON *string* the model passes as the call arguments.
pub fn body_tool_call(id: &str, name: &str, arguments_json: &str) -> String {
    json!({
        "choices": [{
            "message": {
                "role": "assistant",
                "tool_calls": [{
                    "id": id,
                    "function": { "name": name, "arguments": arguments_json }
                }]
            }
        }]
    })
    .to_string()
}

/// The built-in `end_conversation` tool call (terminates the task).
pub fn body_end_conversation(final_message: &str) -> String {
    body_tool_call(
        "e1",
        "end_conversation",
        &json!({ "final_message": final_message }).to_string(),
    )
}

/// A call to the `echo` test tool (see [`EchoTool`]).
pub fn body_echo_call(input: &str) -> String {
    body_tool_call("t1", "echo", &json!({ "input": input }).to_string())
}

/// A call to the `echo` test tool with an explicit `id`, so a test can issue more
/// than one echo call across rounds (distinct tool_call ids).
pub fn body_echo_call_id(id: &str, input: &str) -> String {
    body_tool_call(id, "echo", &json!({ "input": input }).to_string())
}

// ===========================================================================
// HTTP doubles — all strict (panic on an unscripted call)
// ===========================================================================

// `ScriptedHttp` / `CapturingHttp` / `FailingHttp` / `NeverHttp` and the
// `ScriptStep` alias come from claw_interface via the `httpmock` feature.

// ===========================================================================
// LLM builders
// ===========================================================================

fn build_llm<H: ClawHttp>(http: H, supports_tools: bool) -> ClawApi<H> {
    ClawApi::init(
        ClawApiConfig {
            api_key: Some("sk-test".into()),
            backend_type: "openai_compatible".into(),
            model: Some("gpt-test".into()),
            base_url: Some("https://example.invalid".into()),
            supports_tools,
            ..Default::default()
        },
        http,
    )
    .expect("init llm")
}

/// Tool-capable LLM serving the given plain bodies in order (strict).
pub fn scripted_llm(bodies: Vec<String>) -> ClawApi<ScriptedHttp> {
    build_llm(ScriptedHttp::new(bodies), true)
}

/// Tool-capable LLM whose rounds may be successes or transport errors (strict).
pub fn scripted_llm_steps(steps: Vec<ScriptStep>) -> ClawApi<ScriptedHttp> {
    build_llm(ScriptedHttp::with_steps(steps), true)
}

/// Tool-capable LLM that records requests; returns the API plus the capture handle.
pub fn capturing_llm(bodies: Vec<String>) -> (ClawApi<Arc<CapturingHttp>>, Arc<CapturingHttp>) {
    let http = CapturingHttp::new(bodies);
    let llm = build_llm(Arc::clone(&http), true);
    (llm, http)
}

/// Tool-capable LLM whose every round fails.
pub fn failing_llm() -> ClawApi<FailingHttp> {
    build_llm(FailingHttp, true)
}

/// LLM that does NOT support tools, serving the given plain bodies (strict).
pub fn scripted_llm_no_tools(bodies: Vec<String>) -> ClawApi<ScriptedHttp> {
    build_llm(ScriptedHttp::new(bodies), false)
}

/// Tool-capable LLM that must never be called (panics if it is).
pub fn never_called_llm() -> ClawApi<NeverHttp> {
    build_llm(NeverHttp, true)
}

// Filesystem doubles: the shared `DiskFs` from `claw_interface` (the `diskfs`
// dev-dependency feature). `DiskFs::absolute()` backs conversation memory with
// absolute paths; `DiskFs::rooted(base)` keeps skill fixtures portable.

// ===========================================================================
// Tools
// ===========================================================================

/// A trivial caller tool named `echo` that echoes its arguments back.
pub struct EchoTool;

impl ToolHandler for EchoTool {
    fn name(&self) -> &str {
        "echo"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"echo","description":"Echo the arguments back"}}"#
    }

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: format!("echo:{}", call.arguments_json),
            ok: true,
        })
    }
}

// ===========================================================================
// Memory / agent builders
// ===========================================================================

/// A fresh background memory pool.
pub fn test_pool() -> Arc<SharedTaskPool> {
    Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread).expect("memory pool"))
}

/// `<crate>/output/<name>/`, wiped clean and recreated. Use a UNIQUE `name` per
/// test (collisions across tests corrupt each other's transcripts).
pub fn test_output_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("output")
        .join(name);
    std::fs::remove_dir_all(&dir).ok();
    std::fs::create_dir_all(&dir).expect("create output dir");
    dir
}

/// The concrete `ClawFs` these integration tests run over: the real disk
/// backend. `DiskFs` is itself a cheap clone handle, so it needs no `Arc`.
pub type TestFs = DiskFs;

/// A disk-backed [`BaseAgent`] for the integration tests, generic over the HTTP
/// transport `H` the test's LLM double uses.
pub type TestAgent<H> = BaseAgent<H>;

/// A disk-backed [`TranscriptStore`] view for the integration tests.
pub type TestMemory = TranscriptStore<TestFs>;

/// Real disk-backed transcript store.
pub fn test_memory(agent_id: AgentId, dir: impl Into<String>) -> TestMemory {
    TranscriptStore::new(agent_id.0, TranscriptConfig::new(dir), DiskFs::absolute())
}

/// A `BaseAgentBuilder` over a fresh disk transcript store.
pub fn agent_builder<H: ClawHttp>(
    llm: ClawApi<H>,
    agent_id: AgentId,
    dir: impl Into<String>,
) -> BaseAgentBuilder<TestFs, H> {
    BaseAgent::builder(llm, test_memory(agent_id, dir))
}

/// A builder plus a cloned read-only view of the same store, so a test can
/// inspect the committed transcript without going through the agent.
pub fn builder_with_view<H: ClawHttp>(
    llm: ClawApi<H>,
    agent_id: AgentId,
    dir: impl Into<String>,
) -> (BaseAgentBuilder<TestFs, H>, TestMemory) {
    let store = test_memory(agent_id, dir);
    let view = store.clone();
    (BaseAgent::builder(llm, store), view)
}

// ===========================================================================
// Drivers / assertions
// ===========================================================================

/// Pump until the task hands back an answer (`Yielded`) or ends (`Ended`),
/// returning that text. Panics on `Failed` or any other non-progress outcome so
/// an unexpected pause/approval/cancel surfaces instead of hanging.
pub fn run_to_completion<H: ClawHttp>(agent: &mut BaseAgent<H>) -> String {
    loop {
        match agent.tick() {
            TickOutcome::Working => continue,
            TickOutcome::Yielded { text } => return text,
            TickOutcome::Ended { final_message } => return final_message,
            TickOutcome::Failed(error) => panic!("unexpected agent failure: {error}"),
            other => panic!("unexpected outcome: {other:?}"),
        }
    }
}

/// The `content` strings of every message in the committed transcript, in order.
pub fn transcript_contents(view: &TestMemory) -> Vec<String> {
    view.messages()
        .as_array()
        .map(|items| {
            items
                .iter()
                .filter_map(|m| m.get("content").and_then(Value::as_str))
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}
