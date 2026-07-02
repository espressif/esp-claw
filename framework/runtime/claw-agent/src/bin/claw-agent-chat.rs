//! `claw-agent-chat` — a minimal REPL that drives the whole agent system through
//! the public [`claw_agent`] API: build an [`AgentSystem`], create a session,
//! and print each turn's replies.
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

use anyhow::{bail, Result};
use claw_agent::{AgentPersistenceConfig, AgentSystem, BackendKind, ClawApiConfig};

const MEMORY_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/output/claw-agent-chat");
const TRANSCRIPT_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/output/claw-agent-chat/sessions"
);
const PROFILE_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/output/claw-agent-chat/profile"
);
const GLOBAL_LONG_TERM_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/output/claw-agent-chat/long_term/global"
);
const CONVERSATION_LONG_TERM_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/output/claw-agent-chat/long_term/agents/conversation"
);
const WORKER_LONG_TERM_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/output/claw-agent-chat/long_term/agents/worker"
);

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("error: {error}");
        std::process::exit(1);
    }
}

async fn run() -> Result<()> {
    load_env();

    let persistence =
        AgentPersistenceConfig::new(TRANSCRIPT_DIR, PROFILE_DIR, GLOBAL_LONG_TERM_DIR)
            .with_agent_long_term_dir("conversation", CONVERSATION_LONG_TERM_DIR)
            .with_agent_long_term_dir("worker", WORKER_LONG_TERM_DIR);
    let system = AgentSystem::on_disk(llm_config()?, persistence)?;
    let session = system.new_session();

    eprintln!("Session: {}", session.to_wire());
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

        let replies = system.send(session, input).await?;
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
