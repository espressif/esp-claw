//! Interactive chat CLI backed by [`BaseAgent`] with persistent memory.
//!
//! Reads LLM config from `claw-core/.env.local` (same file as the integration
//! tests). Conversation memory is written to `claw-core/output/chat/`.
//!
//! ```
//! cargo run -p claw-agent-cli --bin base-agent --target x86_64-unknown-linux-gnu
//! ```

use std::io::{self, BufRead, Write};

use claw_agent_cli::{load_env, make_llm, make_memory};
use claw_core::agent::{ApprovalDecision, BaseAgent, TickOutcome};

const MEMORY_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../claw-core/output/chat");
const SYSTEM_PROMPT: &str = "You are a helpful, concise assistant.";
const AGENT_ID: usize = 1;

fn main() {
    load_env();

    let (memory, memory_view) = make_memory(AGENT_ID, MEMORY_DIR);
    let mut agent = BaseAgent::builder(make_llm(), memory)
        .with_system_prompt(SYSTEM_PROMPT)
        .build()
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

        if input == "/messages" {
            let messages = memory_view.messages();
            println!(
                "{}",
                serde_json::to_string_pretty(&*messages)
                    .unwrap_or_else(|e| format!("(serialize error: {e})"))
            );
            println!();
            continue;
        }

        agent.run(input);

        loop {
            match agent.tick() {
                TickOutcome::Working => continue,
                TickOutcome::Yielded { text } => {
                    println!("\n{text}\n");
                    break;
                }
                TickOutcome::Ended { final_message } => {
                    println!("\n{final_message}\n");
                    break;
                }
                TickOutcome::Failed(error) => {
                    eprintln!("agent error: {error}");
                    break;
                }
                TickOutcome::AwaitingApproval { id, summary } => {
                    eprintln!("approval requested [{id}]: {summary} — auto-approving");
                    if let Err(error) = agent.resolve_approval(id, ApprovalDecision::Approved) {
                        eprintln!("failed to resolve approval [{id}]: {error}");
                        break;
                    }
                    // Keep pumping so the queued decision resolves next tick.
                }
                TickOutcome::Cancelled { .. } | TickOutcome::Idle => break,
            }
        }
    }

    eprintln!("Goodbye.");
}
