//! Interactive chat CLI backed by [`GenericAgent`] with persistent memory.
//!
//! Drives the single flat agent (here the `conversation` kind) against a live
//! LLM — no semantic FSM, the model drives its own flow. Subagent spawning is
//! left disabled (no registry is wired here), so this exercises the
//! single-agent path. LLM config is read from `claw-core/.env.local`; memory is
//! written to `claw-core/output/generic-chat/`.
//!
//! ```
//! cargo run -p claw-agent-cli --bin generic-agent-chat --target x86_64-unknown-linux-gnu
//! ```

use std::io::{self, BufRead, Write};
use std::sync::Arc;

use claw_agent_cli::{load_env, make_llm_config, make_memory_ingredients};
use claw_core::agent::{
    Agent, AgentCommand, AgentConfig, AgentId, AgentSnapshot, ApprovalDecision, GenericAgent,
    GraphEffect, GraphHost, MapAgentResolver, TickOutcome,
};
use claw_interface::{RealHttp, TokioTimer};
use claw_memory::TranscriptStore;

const MEMORY_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../claw-core/output/generic-chat"
);
const AGENT_ID: u32 = 1;

struct NoopGraphHost;

impl GraphHost for NoopGraphHost {
    fn next_id(&self) -> AgentId {
        AgentId(0)
    }

    fn emit(&self, _requester: AgentId, _effect: GraphEffect) {}

    fn snapshot(&self) -> Vec<AgentSnapshot> {
        Vec::new()
    }
}

#[tokio::main]
async fn main() {
    load_env();

    let (transcript_config, storage) = make_memory_ingredients(MEMORY_DIR);
    let store = TranscriptStore::new(AGENT_ID, transcript_config, storage)
        .expect("failed to open transcript store");
    // The config (system prompt, capabilities, skills) comes from the baked
    // `conversation` manifest. An empty resolver suffices here: the kind declares
    // no capability/skill names, and the base agent merges its own control tools.
    let resolver = MapAgentResolver::new();
    let config =
        AgentConfig::resolve("conversation", &resolver).expect("resolve conversation config");
    // No real orchestrator graph is wired here. The no-op host keeps this manual
    // CLI focused on one agent without exposing the agent's internal transcript.
    let mut agent = GenericAgent::<RealHttp, TokioTimer>::new(
        AgentId(AGENT_ID),
        make_llm_config(),
        store,
        config,
        Arc::new(NoopGraphHost),
        false,
        Arc::from([]),
    )
    .expect("failed to build agent");

    eprintln!("Memory: {MEMORY_DIR}");
    eprintln!("Type your message and press Enter. Empty line or Ctrl-D to quit.\n");

    let stdin = io::stdin();
    loop {
        print!("> ");
        io::stdout().flush().expect("flush stdout");

        let mut line = String::new();
        match stdin.lock().read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let input = line.trim();
        if input.is_empty() {
            break;
        }

        if let Err(error) = agent.send_command(AgentCommand::AppendMessage(input.to_string())) {
            eprintln!("could not accept input: {error}");
            continue;
        }

        loop {
            match agent.tick().await {
                TickOutcome::Working => continue,
                TickOutcome::Yielded { text } => {
                    println!("\n{text}");
                    break;
                }
                TickOutcome::Ended { final_message } => {
                    println!("\n{final_message}");
                    break;
                }
                TickOutcome::Failed(error) => {
                    eprintln!("agent error: {error}");
                    break;
                }
                TickOutcome::AwaitingApproval { id, summary } => {
                    eprintln!("approval requested [{id}]: {summary} — auto-approving");
                    if let Err(error) = agent.send_command(AgentCommand::ApprovalResult {
                        id,
                        decision: ApprovalDecision::Approved,
                    }) {
                        eprintln!("failed to resolve approval [{id}]: {error}");
                        break;
                    }
                    // Keep pumping so the queued decision resolves next tick.
                }
                TickOutcome::Cancelled { .. } | TickOutcome::Idle => break,
            }
        }
        println!();
    }

    eprintln!("Goodbye.");
}
