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
use claw_agent::{
    AgentPersistenceConfig, HostAgentSystem, SessionControl, SessionEvent, SessionEventStream,
};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::{StdThread, TokioExecutor};
use claw_log::{LevelFilter, LogOutput, TracingConfig};
use futures_lite::StreamExt;

const MEMORY_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/output/claw-agent-chat");

struct ChatDriver {
    control: SessionControl,
    events: SessionEventStream,
}

impl ChatDriver {
    fn new(control: SessionControl, events: SessionEventStream) -> Self {
        Self { control, events }
    }

    /// Submit one input and print events until that user-visible turn ends.
    /// Returns whether the turn produced any assistant-visible output.
    async fn send(&mut self, text: impl Into<String>) -> bool {
        if let Err(error) = self.control.submit(text.into()).await {
            print_event("error", &error.to_string(), EventStyle::Error);
            return false;
        }
        let mut saw_output = false;
        while let Some(event) = self.events.next().await {
            match event {
                SessionEvent::Reasoning { text } => {
                    print_event("think", &text, EventStyle::Thinking);
                }
                SessionEvent::Tools { names } => {
                    print_event("tools", &names.join(", "), EventStyle::Tools);
                }
                SessionEvent::Output { text } => {
                    saw_output = true;
                    println!("\n{text}\n");
                }
                SessionEvent::Error { message } => {
                    print_event("error", &message, EventStyle::Error)
                }
                SessionEvent::TurnEnded { .. } | SessionEvent::Closed => break,
                SessionEvent::TurnStarted { .. }
                | SessionEvent::IterationStarted { .. }
                | SessionEvent::IterationEnded => {}
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
    claw_log::init_logger(
        LevelFilter::Info,
        LogOutput::File(Path::new(env!("CARGO_MANIFEST_DIR")).join("../claw-agent/simulator.log")),
    )?;
    claw_log::init_tracing(
        TracingConfig::default()
            .with_context_group_keys("run", ["session", "turn", "agent", "iteration"]),
    )?;
    let env_path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../claw-agent/.env.local");
    if env_path.is_file() {
        if let Err(error) = dotenvy::from_path(&env_path) {
            eprintln!("warning: failed to load {}: {error}", env_path.display());
        }
    }

    let persistence = AgentPersistenceConfig {
        persistence_root: MEMORY_DIR.to_string(),
        skill_roots: Vec::new(),
    };
    let mut llm_config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        required("CLAW_LLM_API_KEY")?,
        required("CLAW_LLM_MODEL")?,
        required("CLAW_LLM_BASE_URL")?,
    );
    llm_config.timeout_ms = 60_000;
    let system = HostAgentSystem::new::<StdThread, TokioExecutor>(llm_config, persistence)?;
    system.start_all()?;
    let session = system.new_session();
    let (control, events) = system.open_session(session)?;
    let mut chat = ChatDriver::new(control, events);

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

/// Read a required, non-empty environment variable or fail with a clear message.
fn required(key: &str) -> Result<String> {
    match std::env::var(key) {
        Ok(value) if !value.is_empty() => Ok(value),
        _ => bail!("{key} must be set (in env or claw-core/.env.local)"),
    }
}
