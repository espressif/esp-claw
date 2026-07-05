//! `claw-agent-chat` — a minimal REPL that drives the whole agent system through
//! the public [`claw_agent`] API: build an [`AgentSystem`], create a session,
//! push channel messages through a registered channel, and print each turn's replies.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`. Memory
//! is written under this crate's `output/claw-agent-chat/`.
//!
//! ```
//! cargo run -p claw-agent --features dev --bin claw-agent-chat --target x86_64-unknown-linux-gnu
//! ```
//!
use std::io::{self, BufRead, Write};
use std::path::Path;
use std::sync::{Arc, Mutex};

use anyhow::{bail, Result};
use claw_agent::{AgentPersistenceConfig, AgentSystem, HostAgentSystem};
use claw_api::{BackendKind, ClawApiConfig};
use claw_channel::{
    Channel, ChannelError, ChannelHandler, ChannelInbound, ChannelOutbound, ChannelResult,
    ChannelRuntime, ChannelSink,
};
use claw_core::SessionId;

const MEMORY_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/output/claw-agent-chat");
const CHANNEL: &str = "claw-agent-chat";
const CHAT_ID: &str = "claw-agent-chat";

struct CliChannel {
    received: Mutex<Vec<String>>,
}

impl CliChannel {
    fn new() -> Self {
        Self {
            received: Mutex::new(Vec::new()),
        }
    }

    fn drain(&self) -> Vec<String> {
        self.received
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .drain(..)
            .collect()
    }
}

struct CliChannelHandler {
    inner: Arc<CliChannel>,
}

impl ChannelHandler for CliChannelHandler {
    fn name(&self) -> &str {
        CHANNEL
    }

    fn start(&self, _sink: ChannelSink) -> ChannelResult<ChannelRuntime> {
        Ok(ChannelRuntime::default())
    }

    fn send(&self, message: ChannelOutbound<'_>) -> ChannelResult<()> {
        self.inner
            .received
            .lock()
            .map_err(|_| ChannelError::new("cli channel lock poisoned"))?
            .push(message.text.unwrap_or_default().to_owned());
        Ok(())
    }
}

struct ChatDriver {
    system: HostAgentSystem,
    session: SessionId,
    channel: Arc<CliChannel>,
    next_message_id: u64,
}

impl ChatDriver {
    fn new(system: HostAgentSystem, session: SessionId, channel: Arc<CliChannel>) -> Self {
        Self {
            system,
            session,
            channel,
            next_message_id: 1,
        }
    }

    async fn send(
        &mut self,
        text: impl Into<String>,
    ) -> Result<Vec<String>, claw_agent::AgentError> {
        let message_number = self.next_message_id;
        self.next_message_id = self.next_message_id.saturating_add(1);
        self.system
            .submit_channel(
                self.session,
                ChannelInbound {
                    channel: CHANNEL.into(),
                    chat_id: CHAT_ID.into(),
                    text: Some(text.into()),
                    attachments: Vec::new(),
                    sender_id: None,
                    message_id: Some(format!("m{message_number}")),
                    correlation_id: None,
                    timestamp_ms: None,
                    target: None,
                    content_type: None,
                    payload_json: None,
                },
            )
            .await?;
        Ok(self.channel.drain())
    }
}

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("error: {error}");
        std::process::exit(1);
    }
}

async fn run() -> Result<()> {
    load_env();

    let persistence = AgentPersistenceConfig::new(MEMORY_DIR);
    let system = AgentSystem::on_disk(llm_config()?, persistence)?;
    let channel = Arc::new(CliChannel::new());
    system
        .channel_registry()
        .register(Channel::from_handler(CliChannelHandler {
            inner: Arc::clone(&channel),
        }))?;
    system.start_all()?;
    let session = system.new_session();
    let mut chat = ChatDriver::new(system, session, channel);

    eprintln!("Memory:  {MEMORY_DIR}");
    eprintln!("Type your message and press Enter. Empty line or Ctrl-D to quit.\n");

    let stdin = io::stdin();
    loop {
        print!("> ");
        io::stdout().flush()?;

        let mut line = String::new();
        if stdin.lock().read_line(&mut line)? == 0 {
            break;
        }
        let input = line.trim();
        if input.is_empty() {
            break;
        }

        let replies = chat.send(input).await?;
        if replies.is_empty() {
            println!("\n(no reply)\n");
        }
        for reply in replies {
            println!("\n{reply}\n");
        }
    }

    eprintln!("Goodbye.");
    Ok(())
}

/// Load `claw-core/.env.local` into the process environment if present. A parse
/// failure is surfaced (not swallowed) but does not abort: the missing variables
/// are then reported precisely by [`llm_config`].
fn load_env() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../claw-core/.env.local");
    if path.is_file() {
        if let Err(error) = dotenvy::from_path(&path) {
            eprintln!("warning: failed to load {}: {error}", path.display());
        }
    }
}

/// Build the LLM client config from the required `CLAW_LLM_*` variables.
fn llm_config() -> Result<ClawApiConfig> {
    let mut config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        required("CLAW_LLM_API_KEY")?,
        required("CLAW_LLM_MODEL")?,
        required("CLAW_LLM_BASE_URL")?,
    );
    config.timeout_ms = 60_000;
    Ok(config)
}

/// Read a required, non-empty environment variable or fail with a clear message.
fn required(key: &str) -> Result<String> {
    match std::env::var(key) {
        Ok(value) if !value.is_empty() => Ok(value),
        _ => bail!("{key} must be set (in env or claw-core/.env.local)"),
    }
}
