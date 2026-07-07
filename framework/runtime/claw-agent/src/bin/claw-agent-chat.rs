//! `claw-agent-chat` — a minimal REPL that drives the whole agent system through
//! the public [`claw_agent`] API: build an [`AgentSystem`], create a session,
//! submit user text, and print each turn's replies.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`. Memory
//! is written under this crate's `output/claw-agent-chat/`.
//!
//! ```
//! cargo run -p claw-agent --features dev --bin claw-agent-chat --target x86_64-unknown-linux-gnu
//! ```
//!
use std::io::{self, BufRead, IsTerminal, Write};
use std::path::Path;

use anstyle::{AnsiColor, Style};
use anyhow::{bail, Result};
use claw_agent::{AgentEvent, AgentPersistenceConfig, AgentSystem, HostAgentSystem};
use claw_api::{BackendKind, ClawApiConfig};
use claw_core::SessionId;
use futures_lite::StreamExt;

const MEMORY_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/output/claw-agent-chat");

struct ChatDriver {
    system: HostAgentSystem,
    session: SessionId,
}

impl ChatDriver {
    fn new(system: HostAgentSystem, session: SessionId) -> Self {
        Self { system, session }
    }

    /// Drive one turn, printing each streamed [`AgentEvent`] as it arrives.
    /// Returns whether the turn produced any assistant-visible output.
    async fn send(&mut self, text: impl Into<String>) -> bool {
        let mut stream = self.system.submit(self.session, text.into());
        let mut saw_output = false;
        while let Some(event) = stream.next().await {
            match event {
                AgentEvent::Reasoning { text } => print_event("think", &text, EventStyle::Thinking),
                AgentEvent::Tools { names } => {
                    print_event("tools", &names.join(", "), EventStyle::Tools);
                }
                AgentEvent::Output { text } => {
                    saw_output = true;
                    println!("\n{text}\n");
                }
                AgentEvent::Error { message } => print_event("error", &message, EventStyle::Error),
                AgentEvent::TurnStarted
                | AgentEvent::TurnEnded
                | AgentEvent::IterationStarted { .. }
                | AgentEvent::IterationEnded => {}
            }
        }
        saw_output
    }
}

enum EventStyle {
    Thinking,
    Tools,
    Error,
}

impl EventStyle {
    fn style(&self) -> Style {
        if !io::stderr().is_terminal() {
            return Style::new();
        }

        match self {
            Self::Thinking => Style::new().dimmed().fg_color(Some(AnsiColor::Cyan.into())),
            Self::Tools => Style::new().bold().fg_color(Some(AnsiColor::Green.into())),
            Self::Error => Style::new().bold().fg_color(Some(AnsiColor::Red.into())),
        }
    }
}

fn print_event(label: &str, message: &str, event_style: EventStyle) {
    let style = event_style.style();
    let mut lines = message.lines();
    let Some(first) = lines.next() else {
        eprintln!("  {style}{label:<5}{style:#}");
        return;
    };

    eprintln!("  {style}{label:<5}{style:#}  {first}");
    for line in lines {
        eprintln!("         {line}");
    }
}

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        print_event("error", &error.to_string(), EventStyle::Error);
        std::process::exit(1);
    }
}

async fn run() -> Result<()> {
    load_env();

    let persistence = AgentPersistenceConfig::new(MEMORY_DIR);
    let system = AgentSystem::on_disk(llm_config()?, persistence)?;
    system.start_all()?;
    let session = system.new_session();
    let mut chat = ChatDriver::new(system, session);

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

        if !chat.send(input).await {
            println!("\n(no reply)\n");
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
