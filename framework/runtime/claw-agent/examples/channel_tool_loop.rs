//! A full channel/tool message loop, end to end and offline.
//!
//! This is the reference for how a device (firmware or host) drives the agent
//! through channels and tools. Everything below uses the `claw_agent` surface:
//!
//! 1. Build an [`AgentSystem`] and register a **tool** plus a **channel**.
//! 2. Start the registered runtime objects.
//! 3. Submit a [`ChannelInbound`] to an explicit session and route the reply back
//!    to the registered channel.
//!
//! The LLM is a scripted in-memory double and the filesystem is in-memory, so the
//! example runs hermetically (no network, no API key):
//!
//! ```bash
//! cargo run -p claw-agent --features dev --example channel_tool_loop \
//!   --target x86_64-unknown-linux-gnu
//! ```

use std::sync::{Arc, Mutex};

use claw_agent::AgentSystem;
use claw_api::{BackendKind, ClawApiConfig};
use claw_channel::{
    Channel, ChannelHandler, ChannelInbound, ChannelOutbound, ChannelResult, ChannelRuntime,
    ChannelSink,
};
use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp};
use claw_tool::{SyncToolHandler, Tool, ToolInvocation, ToolOutput, ToolResult, ToolSpec};

/// The channel id this device talks on. Inbound messages carry it, and replies
/// are routed back to the matching channel.
const LOCAL_CHANNEL: &str = "local";

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

/// A channel backed by an in-memory buffer.
///
/// A real board's channel would frame and write bytes to a transport (BLE, a
/// websocket, a serial link); here `send` just records the reply text so the
/// example can read it back while inbound messages are pushed through
/// `AgentSystem::submit_channel`.
struct LocalChannel {
    id: String,
    received: Arc<Mutex<Vec<String>>>,
}

impl LocalChannel {
    fn new(id: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            received: Arc::new(Mutex::new(Vec::new())),
        }
    }

    /// A shared handle to the replies this channel has delivered.
    fn received(&self) -> Arc<Mutex<Vec<String>>> {
        Arc::clone(&self.received)
    }
}

impl ChannelHandler for LocalChannel {
    fn name(&self) -> &str {
        &self.id
    }

    fn start(&self, _sink: ChannelSink) -> ChannelResult<ChannelRuntime> {
        Ok(ChannelRuntime::default())
    }

    fn send(&self, message: ChannelOutbound<'_>) -> ChannelResult<()> {
        if let Ok(mut received) = self.received.lock() {
            received.push(message.text.unwrap_or_default().to_owned());
        }
        Ok(())
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
    // 1. Describe the device as a tool plus a channel.
    let local = LocalChannel::new(LOCAL_CHANNEL);
    let replies = local.received();

    // 2. Build the system. Hermetic backends (in-memory fs + scripted LLM) keep
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
    system
        .channel_registry()
        .register(Channel::from_handler(local))?;
    println!("registered tool `time_now` and channel `{LOCAL_CHANNEL}`");
    system.start_all()?;
    let session = system.new_session();

    // 3. Drive the loop: push an inbound message as the channel would. The
    //    explicit session id selects the agent session.
    system
        .submit_channel(
            session,
            ChannelInbound {
                channel: LOCAL_CHANNEL.into(),
                chat_id: "chat".into(),
                text: Some("Hi, what time is it?".into()),
                attachments: Vec::new(),
                sender_id: Some("user".into()),
                message_id: Some("m1".into()),
                correlation_id: None,
                timestamp_ms: None,
                target: None,
                content_type: None,
                payload_json: None,
            },
        )
        .await?;

    // The reply is delivered by the time `submit_channel` returns.
    let delivered = replies
        .lock()
        .map_err(|_| anyhow::anyhow!("reply buffer poisoned"))?;
    println!(
        "\nchannel `{LOCAL_CHANNEL}` received {} reply(ies):",
        delivered.len()
    );
    for reply in delivered.iter() {
        println!("  > {reply}");
    }
    assert_eq!(delivered.len(), 1, "expected exactly one routed reply");

    Ok(())
}
