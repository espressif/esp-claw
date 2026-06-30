//! Interactive chat CLI backed by the full [`Orchestrator`] (Layer 1).
//!
//! Unlike `generic-agent-chat` (which drives a single [`GenericAgent`]
//! directly), this goes through the real orchestrator path: one session, a
//! per-session agent graph built by [`FsAgentFactory`], replies routed back
//! through the channel egress. The root is a `conversation` agent that can spawn
//! `worker` subagents, so this exercises multi-agent spawning end to end.
//!
//! Capabilities/skills come from each kind's compile-time manifest; the resolver
//! is empty here (the built-in kinds declare no extra capabilities — agents still
//! get their built-in control/spawn tools). Approval requests are surfaced as
//! ordinary messages; this simple loop does not resolve them (matching the other
//! CLIs).
//!
//! LLM config is read from `claw-core/.env.local`; memory is written to
//! `claw-core/output/orchestrator-chat/`.
//!
//! ```
//! cargo run -p claw-agent-cli --bin orchestrator-chat --target x86_64-unknown-linux-gnu
//! ```
//!
//! Pass `--log-file <PATH>` to redirect all log/trace output to a file
//! (overwritten, plain text); without it, output goes to stderr as before. The
//! interactive prompt and replies always stay on the console.

use std::io::{self, BufRead, Write};
use std::sync::Arc;

use claw_agent_cli::{
    load_env, make_compaction, make_llm_config, make_long_term_deps, make_memory_fs, CliFs,
};
use claw_core::agent::{FsAgentFactory, MapAgentResolver};
use claw_core::{
    ChannelEgress, ChannelEgressHub, ChannelIngressSink, ChannelTransport, InboundMessage,
    Orchestrator, RecordingTransport,
};
use claw_interface::RealHttp;

const MEMORY_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../claw-core/output/orchestrator-chat"
);
/// The CLI's single inbound/outbound channel; the transport registers under it
/// and inbound messages carry it so the reply route resolves back to us.
const CHANNEL: &str = "cli";
const CHAT_ID: &str = "cli-chat";

/// Parse the optional `--log-file <PATH>` (or `--log-file=<PATH>`) flag into a
/// [`claw_log::LogOutput`]; absent → [`claw_log::LogOutput::Stderr`]. Exits with a
/// usage error when the flag is given without a path.
fn log_output_from_args() -> claw_log::LogOutput {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if let Some(path) = arg.strip_prefix("--log-file=") {
            return claw_log::LogOutput::File(path.into());
        }
        if arg == "--log-file" {
            match args.next() {
                Some(path) => return claw_log::LogOutput::File(path.into()),
                None => {
                    eprintln!("error: --log-file requires a path");
                    std::process::exit(2);
                }
            }
        }
    }
    claw_log::LogOutput::Stderr
}

fn main() {
    load_env();

    // `--log-file <PATH>` redirects all log/trace output to a file (overwritten);
    // without it, output goes to stderr as before. The chat prompt/replies always
    // stay on the console.
    let log_output = log_output_from_args();
    let log_file_path = match &log_output {
        claw_log::LogOutput::File(path) => Some(path.display().to_string()),
        claw_log::LogOutput::Stderr => None,
    };

    // Install the flat-tree `tracing` subscriber so the layered spans/events
    // (session > turn > agent > iteration_loop) print as `TRACE …` lines on the
    // chosen sink — the same stream a device build sends to `ESP_LOGx`.
    // `init_logger` routes plain `log::` records to that sink too.
    if let Err(error) = claw_log::init_logger(claw_log::LevelFilter::Trace, log_output) {
        eprintln!("failed to initialize logging: {error}");
        std::process::exit(1);
    }
    // The caller owns the inherited-context groups; claw_core uses the
    // `conversation` group (session > turn > agent > iteration).
    claw_log::init_tracing(
        claw_log::TracingConfig::default()
            .with_context_group_keys("conversation", ["session", "turn", "agent", "iteration"]),
    )
    .expect("install tracing subscriber");

    // Empty resolver: the built-in conversation/worker manifests declare no extra
    // capabilities, so no name->handler mapping is needed yet.
    let resolver = Arc::new(MapAgentResolver::new());
    let factory = Arc::new(FsAgentFactory::<CliFs, RealHttp>::new(
        resolver,
        make_llm_config(true),
        MEMORY_DIR,
        make_memory_fs(),
        make_compaction(),
        make_long_term_deps(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../claw-core/output/orchestrator-chat/long_term"
        )),
    ));

    // A recording transport doubles as the CLI's "screen": the orchestrator sends
    // replies through the egress, and we drain them after each turn.
    let transport = RecordingTransport::new(CHANNEL);
    let egress = Arc::new(ChannelEgressHub::new());
    egress.register(Arc::clone(&transport) as Arc<dyn ChannelTransport>);

    let orchestrator = Orchestrator::builder()
        .config_egress(egress as Arc<dyn ChannelEgress>)
        .with_agent_factory(factory)
        .build();
    let session = orchestrator.session_create();

    eprintln!("Memory:  {MEMORY_DIR}");
    if let Some(path) = &log_file_path {
        eprintln!("Logs:    {path}");
    }
    eprintln!("Session: {}", session.to_wire());
    eprintln!("Type your message and press Enter. Empty line or Ctrl-D to quit.\n");

    let stdin = io::stdin();
    let mut turn: u64 = 0;
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

        turn += 1;
        orchestrator.push_user_message(InboundMessage {
            message_id: format!("m{turn}"),
            channel: CHANNEL.into(),
            chat_id: CHAT_ID.into(),
            sender_id: None,
            session_id: session.to_wire(),
            text: input.to_string(),
        });

        // The orchestrator drives the graph synchronously inside `push_user_message`
        // and routes every reply/approval through our transport.
        let replies = transport.drain_sent();
        if replies.is_empty() {
            println!("\n(no reply)");
        }
        for reply in replies {
            println!("\n{}", reply.text);
        }
        println!();
    }

    eprintln!("Goodbye.");
}
