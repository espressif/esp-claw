//! A full tool/submit message loop, end to end and offline.
//!
//! This is the reference for how a device (firmware or host) drives the agent
//! through direct session submission plus tools. Everything below uses the
//! `claw_agent` surface:
//!
//! 1. Build an [`AgentSystem`] and register a **tool**.
//! 2. Start the registered runtime objects.
//! 3. Submit user text to an explicit session and read the returned replies.
//!
//! The LLM is a scripted in-memory double and the filesystem is in-memory, so the
//! example runs hermetically (no network, no API key):
//!
//! ```bash
//! cargo run -p claw-agent --features dev --example tool_submit_loop \
//!   --target x86_64-unknown-linux-gnu
//! ```

use claw_agent::{AgentEvent, AgentSystem};
use claw_api::{BackendKind, ClawApiConfig};
use claw_core::DeliveryKind;
use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp};
use claw_tool::{SyncToolHandler, Tool, ToolInvocation, ToolOutput, ToolResult, ToolSpec};
use futures_lite::StreamExt;

/// A tool: returns a fixed timestamp. Registering it makes `time_now`
/// resolvable by the agent; whether the model calls it is up to the prompt.
struct TimeNowTool;

impl ToolSpec for TimeNowTool {
    fn name(&self) -> &str {
        "time_now"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"time_now","description":"Current time","parameters":{"type":"object","properties":{}}}}"#
    }
}

impl SyncToolHandler for TimeNowTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        Ok(ToolOutput {
            output: "2026-06-29T17:00:00Z".into(),
            ok: true,
        })
    }
}

/// A scripted assistant turn returning plain text (no tool call this round).
fn assistant_text(text: &str) -> String {
    serde_json::json!({
        "choices": [{ "message": { "role": "assistant", "content": text } }]
    })
    .to_string()
}

/// A test LLM config; its base URL is never dialed (HTTP is the scripted double).
fn scripted_llm() -> ClawApiConfig {
    ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-example",
        "gpt-example",
        "https://example.invalid",
    )
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // 1. Build the system. Hermetic backends (in-memory fs + scripted LLM) keep
    //    the example offline and deterministic.
    SharedScriptHttp::install(vec![assistant_text(
        "Hello from the agent — the local time is 2026-06-29T17:00:00Z.",
    )]);

    let system = AgentSystem::<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>::new(
        scripted_llm(),
        claw_agent::AgentPersistenceConfig::new("/mem"),
    )?;
    system
        .tool_registry()
        .register(Tool::from_sync(TimeNowTool))?;
    println!("registered tool `time_now`");
    system.start_all()?;
    let session = system.new_session();

    // 2. Drive the loop: explicit session id selects the agent session. `submit`
    //    returns a stream of `AgentEvent`s; draining it runs the turn.
    let mut stream = system.submit(
        session,
        "Hi, what time is it?".to_string(),
        DeliveryKind::Interrupt,
    );

    println!("\nsession `{session}` events:");
    let mut outputs = Vec::new();
    while let Some(event) = stream.next().await {
        match event {
            AgentEvent::Output { text } => {
                println!("  > {text}");
                outputs.push(text);
            }
            AgentEvent::Reasoning { text } => println!("  [thinking] {text}"),
            AgentEvent::Tools { names } => println!("  [tools] {}", names.join(", ")),
            AgentEvent::Error { message } => println!("  [error] {message}"),
            other => println!("  [{other:?}]"),
        }
    }
    assert_eq!(outputs.len(), 1, "expected exactly one output");

    Ok(())
}
