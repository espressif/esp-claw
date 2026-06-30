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

use claw_agent_cli::{load_env, make_llm, make_memory_ingredients};
use claw_core::agent::{
    Agent, AgentCommand, AgentConfig, AgentId, GenericAgent, MapAgentResolver, TickOutcome,
};
use claw_core::History;
use owo_colors::OwoColorize;
use serde_json::Value;

const MEMORY_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../claw-core/output/generic-chat"
);
const AGENT_ID: usize = 1;

fn main() {
    load_env();

    let (mem_config, mem_deps) = make_memory_ingredients(MEMORY_DIR);
    // The config (system prompt, capabilities, skills) comes from the baked
    // `conversation` manifest. An empty resolver suffices here: the kind declares
    // no capability/skill names, and the base agent merges its own control tools.
    let resolver = MapAgentResolver::new();
    let config =
        AgentConfig::resolve("conversation", &resolver).expect("resolve conversation config");
    // No graph host is wired here, so the single agent gets neither the
    // `spawn_subagent` nor `respond_to_approval` tool — this is the single-agent
    // path on purpose.
    let mut agent = GenericAgent::new(
        AgentId(AGENT_ID),
        make_llm(true),
        mem_config,
        mem_deps,
        config,
        None,
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

        if input == "/messages" {
            let messages = agent.history().messages();
            println!(
                "{}",
                serde_json::to_string_pretty(&*messages)
                    .unwrap_or_else(|e| format!("(serialize error: {e})"))
            );
            println!();
            continue;
        }

        // Remember where the transcript ends so we can show the tool calls this
        // turn appends, after the reply.
        let turn_start = message_count(agent.history());

        if let Err(error) = agent.send_command(AgentCommand::AppendMessage(input.to_string())) {
            eprintln!("could not accept input: {error}");
            continue;
        }

        loop {
            match agent.tick() {
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
                    // The Agent trait does not expose approval resolution, so this
                    // CLI cannot grant it; report and return to the prompt.
                    eprintln!(
                        "approval requested [{id}]: {summary} — not supported in this CLI; \
                         returning to prompt"
                    );
                    break;
                }
                TickOutcome::Cancelled { .. } | TickOutcome::Idle => break,
            }
        }

        print_tool_calls_since(&agent.history().messages(), turn_start);
        println!();
    }

    eprintln!("Goodbye.");
}

/// Number of messages currently in the transcript.
fn message_count(history: &dyn History) -> usize {
    history.messages().as_array().map_or(0, Vec::len)
}

/// Print, in gray under the model's reply, every tool call recorded after index
/// `start` — the calls the model made while producing this turn's answer.
fn print_tool_calls_since(messages: &Value, start: usize) {
    let Some(items) = messages.as_array() else {
        return;
    };
    for message in items.iter().skip(start) {
        let Some(calls) = message.get("tool_calls").and_then(Value::as_array) else {
            continue;
        };
        for call in calls {
            let function = call.get("function");
            let name = function
                .and_then(|f| f.get("name"))
                .and_then(Value::as_str)
                .unwrap_or_default();
            if name.is_empty() {
                continue;
            }
            let arguments = match function.and_then(|f| f.get("arguments")) {
                Some(Value::String(s)) => s.clone(),
                Some(value) => value.to_string(),
                None => String::new(),
            };
            println!("{}", format!("  ↳ {name}({arguments})").bright_black());
        }
    }
}
