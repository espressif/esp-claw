//! A full capability-based message loop, end to end and offline.
//!
//! This is the reference for how a device (firmware or host) drives the agent
//! through *capabilities only*. Everything below uses the `claw_agent` surface:
//!
//! 1. Build a [`Registry`] and register two [`Capability`]s — a **tool** and a
//!    **channel** — exactly as a board would at boot.
//! 2. Hand the registry to [`AgentSystem`] via
//!    [`builder().capabilities(...)`](claw_agent::AgentSystemBuilder::capabilities).
//!    The registry's tools become the agent's resolver and its channels become
//!    outbound transports — no manual wiring.
//! 3. The channel capability ([`ChannelAdapter`]) pushes an inbound user message
//!    through [`AgentSystem::ingress`] and captures the reply the orchestrator
//!    routes back to it.
//!
//! The LLM is a scripted in-memory double and the filesystem is in-memory, so the
//! example runs hermetically (no network, no API key):
//!
//! ```bash
//! cargo run -p claw-agent --example capability_loop \
//!   --target x86_64-unknown-linux-gnu
//! ```

use std::sync::{Arc, Mutex};

use claw_agent::{
    AgentSystem, BackendKind, Capability, CapabilityError, ChannelAdapter, ClawApiConfig,
    InboundMessage, OutboundMessage, PoolConfig, Registry, SharedTaskPool, Tool, ToolHandler,
    ToolInvocation, ToolInvokeError, ToolOutput,
};
use claw_interface::{BlockingClawHttpAsync, ImmediateTimer, MemFs, SharedScriptHttp, StdThread};

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
/// example can read it back. This is the *outbound* half of the channel.
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
    // Run each capability's lifecycle `start` hook, as a board would at boot.
    registry.start_all()?;
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
    let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
    SharedScriptHttp::install(vec![assistant_text(
        "Hello from the agent — the local time is 2026-06-29T17:00:00Z.",
    )]);

    let system =
        AgentSystem::builder::<MemFs, BlockingClawHttpAsync<SharedScriptHttp>, ImmediateTimer>()
            .llm(scripted_llm())
            .memory_dir("/mem/agents")
            .task_pool(pool)
            .capabilities(Arc::clone(&registry))
            .build()?;

    // 3. Drive the loop: open a session, then push an inbound message *as the
    //    channel would*. The reply is routed back out to `LocalChannel::send`.
    let session = system.new_session();
    system
        .ingress()
        .push_user_message(InboundMessage {
            message_id: "m1".into(),
            channel: LOCAL_CHANNEL.into(),
            chat_id: session.to_wire(),
            sender_id: Some("user".into()),
            session_id: session.to_wire(),
            text: "Hi, what time is it?".into(),
        })
        .await;

    // The orchestrator drives the turn synchronously, so the reply has already
    // been delivered to the channel by the time `push_user_message` returns.
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
