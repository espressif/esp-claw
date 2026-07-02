//! A full capability-based message loop, end to end and offline.
//!
//! This is the reference for how a device (firmware or host) drives the agent
//! through *capabilities only*. Everything below uses the `claw_agent` surface:
//!
//! 1. Build a [`Registry`] and register two [`Capability`]s — a **tool** and a
//!    **channel** — exactly as a board would at boot.
//! 2. Hand the registry to [`AgentSystem::new`]. The registry's tools become
//!    the agent's resolver and its channels become outbound transports — no
//!    manual wiring.
//! 3. The channel capability ([`ChannelAdapter`]) is opened by the system and an
//!    inbound message submitted through [`AgentSystem::push_message`] is routed
//!    back to it.
//!
//! The LLM is a scripted in-memory double and the filesystem is in-memory, so the
//! example runs hermetically (no network, no API key):
//!
//! ```bash
//! cargo run -p claw-agent --features dev --example capability_loop \
//!   --target x86_64-unknown-linux-gnu
//! ```

use std::sync::{Arc, Mutex};

use claw_agent::{
    init_tool_executor, AgentSystem, BackendKind, Capability, CapabilityError, ChannelAdapter,
    ChannelRuntime, ClawApiConfig, InboundMessage, OutboundMessage, Registry, Tool, ToolHandler,
    ToolInvocation, ToolInvokeError, ToolOutput,
};
use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp, StdThread};

/// The channel id this device talks on. Inbound messages carry it, and replies
/// are routed back to the matching channel capability.
const LOCAL_CHANNEL: &str = "local";

/// A tool capability: returns a fixed timestamp. Registering it makes `time_now`
/// resolvable by the agent; whether the model calls it is up to the prompt.
struct TimeNowTool;

impl ToolHandler for TimeNowTool {
    fn name(&self) -> &str {
        "time_now"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"time_now","description":"Current time","parameters":{"type":"object","properties":{}}}}"#
    }

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: "2026-06-29T17:00:00Z".into(),
            ok: true,
        })
    }
}

/// A channel capability backed by an in-memory buffer.
///
/// A real board's channel would frame and write bytes to a transport (BLE, a
/// websocket, a serial link); here `send` just records the reply text so the
/// example can read it back while inbound messages are pushed through
/// `AgentSystem::push_message`.
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

impl ChannelAdapter for LocalChannel {
    fn channel_id(&self) -> &str {
        &self.id
    }

    fn open(&self, _runtime: Arc<dyn ChannelRuntime>) -> Result<(), CapabilityError> {
        Ok(())
    }

    fn close(&self) -> Result<(), CapabilityError> {
        Ok(())
    }

    fn send(&self, message: &OutboundMessage) -> Result<(), CapabilityError> {
        // A real adapter would never panic on a poisoned lock; this example keeps
        // it simple. Surface the reply instead of dropping it.
        if let Ok(mut received) = self.received.lock() {
            received.push(message.text.clone());
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
    // 1. Describe the device as capabilities.
    let local = Arc::new(LocalChannel::new(LOCAL_CHANNEL));
    let replies = local.received();

    let registry = Arc::new(Registry::new());
    registry.register(Capability::tool(Tool::new(TimeNowTool)))?;
    registry.register(Capability::channel(
        Arc::clone(&local) as Arc<dyn ChannelAdapter>
    ))?;
    println!(
        "registered {} tool(s) and {} channel(s)",
        registry.tools().len(),
        registry.channels().len()
    );
    println!(
        "`time_now` resolves: {}",
        registry.tool("time_now").is_some()
    );

    // 2. Build the system from the registry. Hermetic backends (in-memory fs +
    //    scripted LLM) keep the example offline and deterministic.
    init_tool_executor(StdThread)?;
    SharedScriptHttp::install(vec![assistant_text(
        "Hello from the agent — the local time is 2026-06-29T17:00:00Z.",
    )]);

    let system = AgentSystem::new::<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>(
        scripted_llm(),
        claw_agent::AgentPersistenceConfig::new("/mem"),
        Arc::clone(&registry),
    )?;
    system.start()?;
    let session = system.new_session();
    system.bind_session(session, LOCAL_CHANNEL, "chat")?;

    // 3. Drive the loop: push an inbound message as the channel would. The
    //    explicit (channel, chat_id) session binding routes the reply back out
    //    to `LocalChannel::send`.
    system
        .push_message(InboundMessage {
            message_id: "m1".into(),
            channel: LOCAL_CHANNEL.into(),
            chat_id: "chat".into(),
            sender_id: Some("user".into()),
            text: "Hi, what time is it?".into(),
        })
        .await?;

    // The system drives the turn synchronously, so the reply has already been
    // delivered to the channel by the time `push_message` returns.
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
