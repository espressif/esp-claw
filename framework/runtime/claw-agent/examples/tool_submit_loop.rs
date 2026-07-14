//! A full tool/submit message loop, end to end and offline.
//!
//! This is the reference for how a device (firmware or host) drives the agent
//! through direct session submission plus tools. Everything below uses the
//! `claw_agent` surface:
//!
//! 1. Build an [`AgentSystem`] and register a **tool**.
//! 2. Start the registered runtime objects.
//! 3. Open a session, submit user text through its control half, and read the
//!    returned replies from its event half.
//!
//! The LLM is a scripted in-memory double and the filesystem is in-memory, so the
//! example runs hermetically (no network, no API key):
//!
//! ```bash
//! cargo run -p claw-agent --features dev --example tool_submit_loop \
//!   --target x86_64-unknown-linux-gnu
//! ```

use claw_agent::{AgentSystem, Message, SessionEvent, SessionPersistence, StreamPart};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::{
    BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp, StdThread, TokioExecutor,
};
use claw_log::{LevelFilter, LogOutput, TracingConfig};
use claw_tool::{
    SyncToolHandler, Tool, ToolGroup, ToolInvocation, ToolOutput, ToolResult, ToolSpec,
};
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
    claw_log::init_logger(LevelFilter::Info, LogOutput::Stderr)?;
    claw_log::init_tracing(
        TracingConfig::default()
            .with_context_group_keys("run", ["system", "session", "turn", "agent", "iteration"]),
    )?;

    // 1. Build the system. Hermetic backends (in-memory fs + scripted LLM) keep
    //    the example offline and deterministic.
    SharedScriptHttp::install(vec![assistant_text(
        "Hello from the agent — the local time is 2026-06-29T17:00:00Z.",
    )]);

    let system = AgentSystem::<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>::new::<
        StdThread,
        TokioExecutor,
    >(
        scripted_llm(),
        claw_agent::AgentPersistenceConfig {
            persistence_root: "/mem".to_string(),
            skill_roots: Vec::new(),
        },
    )?;
    system.tool_registry().register_group(ToolGroup::new(
        "example",
        true,
        [Tool::from_sync(TimeNowTool)],
    ))?;
    println!("registered tool `time_now`");
    system.start_all()?;
    let session = system.new_session(SessionPersistence::Persistent);

    // 2. Drive the loop: explicit session id selects the agent session.
    let (control, mut events) = system.open_session(session)?;
    control
        .submit(Message::text("Hi, what time is it?"))
        .await?;

    println!("\nsession `{session}` events:");
    let mut outputs = Vec::new();
    while let Some(event) = events.next().await {
        match event {
            SessionEvent::Output(StreamPart::Delta(text)) => {
                println!("  > {text}");
                outputs.push(text);
            }
            SessionEvent::Reasoning(StreamPart::Delta(text)) => println!("  [thinking] {text}"),
            SessionEvent::ToolCalls(StreamPart::Delta(call)) => {
                println!("  [tools] {}", call.name)
            }
            SessionEvent::Reasoning(StreamPart::End)
            | SessionEvent::Output(StreamPart::End)
            | SessionEvent::ToolCalls(StreamPart::End) => {}
            SessionEvent::Error { message } => println!("  [error] {message}"),
            SessionEvent::TurnEnded { .. } => break,
            other => println!("  [{other:?}]"),
        }
    }
    assert_eq!(outputs.len(), 1, "expected exactly one output");

    Ok(())
}
