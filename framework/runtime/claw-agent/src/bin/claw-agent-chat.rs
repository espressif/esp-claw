//! `claw-agent-chat` — a minimal REPL that drives the whole agent system through
//! the public [`claw_agent`] API: build an [`AgentSystem`], create a session,
//! submit user text, and print each turn's replies.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`. Memory
//! is written under this crate's `output/claw-agent-chat/`.
//!
//! ```
//! cargo run -p claw-agent --features dev,cache_profile --bin claw-agent-chat
//! ```
//!
#[path = "claw-agent-chat/command.rs"]
mod command;
#[path = "claw-agent-chat/line_editor.rs"]
mod line_editor;

use std::future::Future;
use std::io::{self, IsTerminal, Write};
use std::path::Path;

use anstyle::{AnsiColor, Style};
use anyhow::{bail, Result};
use claw_agent::{
    AgentPersistenceConfig, HostAgentSystem, Message, PermissionLevel, SessionControl,
    SessionEvent, SessionEventStream, SessionPersistence, StreamPart, ToolCall, TurnOrigin,
};
use claw_api::{ApiUsage, BackendKind, ClawApiConfig};
use claw_interface::{StdThread, TokioExecutor};
use claw_log::{LevelFilter, LogOutput, TracingConfig};
use futures_lite::StreamExt;

use command::{parse_input, CliInput};
use line_editor::{ChatLineEditor, LineInput};

const MEMORY_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/output/claw-agent-chat");

struct ChatDriver {
    control: SessionControl,
    events: SessionEventStream,
    total_usage: ApiUsage,
    content: ContentRenderer,
    active_origin: Option<TurnOrigin>,
    saw_output: bool,
}

impl ChatDriver {
    fn new(control: SessionControl, events: SessionEventStream) -> Self {
        Self {
            control,
            events,
            total_usage: ApiUsage::default(),
            content: ContentRenderer::default(),
            active_origin: None,
            saw_output: false,
        }
    }

    fn total_usage(&self) -> Option<ApiUsage> {
        has_usage(self.total_usage).then_some(self.total_usage)
    }

    async fn submit(&self, text: impl Into<String>) -> bool {
        if let Err(error) = self.control.submit(Message::text(text)).await {
            print_event("error", &error.to_string(), EventStyle::Error);
            return false;
        }
        true
    }

    async fn set_permission_level(&self, level: PermissionLevel) -> bool {
        if let Err(error) = self.control.set_permission_level(level).await {
            print_event("error", &error.to_string(), EventStyle::Error);
            return false;
        }
        print_event(
            "permission",
            &format!("set to {}", permission_level_name(level)),
            EventStyle::Permission,
        );
        true
    }

    fn render(
        &mut self,
        event: SessionEvent,
        editor: &mut ChatLineEditor,
        above_prompt: bool,
    ) -> Result<RenderOutcome> {
        let outcome = match event {
            SessionEvent::TurnStarted { origin, .. } => {
                self.content.start_turn(above_prompt, editor)?;
                self.active_origin = Some(origin);
                self.saw_output = false;
                RenderOutcome::TurnStarted
            }
            SessionEvent::Reasoning(part) => {
                self.content.reasoning(part)?;
                RenderOutcome::Continue
            }
            SessionEvent::Output(part) => {
                self.saw_output |= self.content.output(part)?;
                RenderOutcome::Continue
            }
            SessionEvent::ToolCalls(part) => {
                self.content.tool_calls(part, editor)?;
                RenderOutcome::Continue
            }
            SessionEvent::Usage { usage } => {
                accumulate_usage(&mut self.total_usage, usage);
                self.content.finish(editor)?;
                self.content
                    .event(editor, "usage", &format_usage(usage), EventStyle::Usage)?;
                RenderOutcome::Continue
            }
            SessionEvent::Error { message } => {
                self.content.finish(editor)?;
                self.content
                    .event(editor, "error", &message, EventStyle::Error)?;
                RenderOutcome::Continue
            }
            SessionEvent::TurnEnded { .. } => {
                self.content.finish(editor)?;
                let user = matches!(self.active_origin.take(), Some(TurnOrigin::User));
                let saw_output = std::mem::take(&mut self.saw_output);
                RenderOutcome::TurnEnded { user, saw_output }
            }
            SessionEvent::Closed => {
                self.content.finish(editor)?;
                RenderOutcome::Closed
            }
            SessionEvent::IterationStarted { .. } | SessionEvent::IterationEnded => {
                RenderOutcome::Continue
            }
        };
        Ok(outcome)
    }
}

enum RenderOutcome {
    Continue,
    TurnStarted,
    TurnEnded { user: bool, saw_output: bool },
    Closed,
}

enum IdleActivity {
    Input(Option<LineInput>),
    Session(Option<SessionEvent>),
}

async fn next_idle_activity(
    input: impl Future<Output = Option<LineInput>>,
    event: impl Future<Output = Option<SessionEvent>>,
) -> IdleActivity {
    futures_lite::future::race(
        async move { IdleActivity::Input(input.await) },
        async move { IdleActivity::Session(event.await) },
    )
    .await
}

#[derive(Default)]
struct ContentRenderer {
    reasoning: LineState,
    output: LineState,
    above_prompt: bool,
    buffer: String,
}

impl ContentRenderer {
    fn start_turn(&mut self, above_prompt: bool, editor: &mut ChatLineEditor) -> Result<()> {
        self.finish(editor)?;
        self.above_prompt = above_prompt;
        Ok(())
    }

    fn reasoning(&mut self, part: StreamPart<String>) -> Result<()> {
        match part {
            StreamPart::Delta(fragment) => self.reasoning_delta(&fragment)?,
            StreamPart::End => self.finish_reasoning(),
        }
        Ok(())
    }

    fn output(&mut self, part: StreamPart<String>) -> Result<bool> {
        match part {
            StreamPart::Delta(fragment) => {
                self.finish_reasoning();
                self.output.observe(&fragment);
                if self.above_prompt {
                    self.buffer.push_str(&fragment);
                } else {
                    print!("{fragment}");
                    io::stdout().flush()?;
                }
                Ok(true)
            }
            StreamPart::End => {
                self.finish_output();
                Ok(false)
            }
        }
    }

    fn tool_calls(
        &mut self,
        part: StreamPart<ToolCall>,
        editor: &mut ChatLineEditor,
    ) -> Result<()> {
        self.finish(editor)?;
        if let StreamPart::Delta(call) = part {
            self.event(editor, "tools", &call.name, EventStyle::Tools)?;
        }
        Ok(())
    }

    fn event(
        &mut self,
        editor: &mut ChatLineEditor,
        label: &str,
        message: &str,
        style: EventStyle,
    ) -> Result<()> {
        if self.above_prompt {
            editor.print(format_event(label, message, style))?;
        } else {
            print_event(label, message, style);
        }
        Ok(())
    }

    fn reasoning_delta(&mut self, fragment: &str) -> Result<()> {
        if fragment.is_empty() {
            return Ok(());
        }

        let style = EventStyle::Thinking.style();
        if !self.reasoning.is_open() {
            if self.above_prompt {
                self.buffer
                    .push_str(&format!("  {style}{:<5}{style:#}  ", "think"));
            } else {
                eprint!("  {style}{:<5}{style:#}  ", "think");
            }
        }
        self.reasoning.observe(fragment);
        if self.above_prompt {
            self.buffer.push_str(fragment);
        } else {
            eprint!("{fragment}");
            io::stderr().flush()?;
        }
        Ok(())
    }

    fn finish(&mut self, editor: &mut ChatLineEditor) -> Result<()> {
        self.finish_reasoning();
        self.finish_output();
        if self.above_prompt && !self.buffer.is_empty() {
            let output = std::mem::take(&mut self.buffer);
            print_above_prompt(editor, output)?;
        }
        Ok(())
    }

    fn finish_reasoning(&mut self) {
        if self.above_prompt {
            finish_buffered_line(&mut self.reasoning, &mut self.buffer);
        } else {
            finish_line(&mut self.reasoning, io::stderr());
        }
    }

    fn finish_output(&mut self) {
        if self.above_prompt {
            finish_buffered_line(&mut self.output, &mut self.buffer);
        } else {
            finish_line(&mut self.output, io::stdout());
        }
    }
}

#[derive(Default)]
struct LineState {
    open: bool,
    needs_newline: bool,
}

impl LineState {
    fn is_open(&self) -> bool {
        self.open
    }

    fn observe(&mut self, fragment: &str) {
        self.open = true;
        self.needs_newline = !fragment.ends_with('\n');
    }

    fn finish(&mut self) -> Option<bool> {
        if !self.open {
            return None;
        }
        let needs_newline = self.needs_newline;
        *self = Self::default();
        Some(needs_newline)
    }
}

fn finish_line(line: &mut LineState, mut writer: impl Write) {
    let Some(needs_newline) = line.finish() else {
        return;
    };
    if needs_newline {
        let _ = writeln!(writer);
    } else {
        let _ = writer.flush();
    }
}

fn finish_buffered_line(line: &mut LineState, buffer: &mut String) {
    if line.finish() == Some(true) {
        buffer.push('\n');
    }
}

enum EventStyle {
    Thinking,
    Tools,
    Permission,
    Usage,
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
            Self::Permission => Style::new().fg_color(Some(AnsiColor::Cyan.into())),
            Self::Usage => Style::new()
                .dimmed()
                .fg_color(Some(AnsiColor::Yellow.into())),
            Self::Error => Style::new().bold().fg_color(Some(AnsiColor::Red.into())),
        }
    }
}

fn print_event(label: &str, message: &str, event_style: EventStyle) {
    eprintln!("{}", format_event(label, message, event_style));
}

fn format_event(label: &str, message: &str, event_style: EventStyle) -> String {
    let style = event_style.style();
    let mut lines = message.lines();
    let Some(first) = lines.next() else {
        return format!("  {style}{label:<5}{style:#}");
    };

    let mut rendered = format!("  {style}{label:<5}{style:#}  {first}");
    for line in lines {
        rendered.push_str("\n         ");
        rendered.push_str(line);
    }
    rendered
}

fn print_above_prompt(editor: &mut ChatLineEditor, message: String) -> Result<()> {
    let message = message.trim_end_matches(['\r', '\n']);
    if !message.is_empty() {
        editor.print(message.to_string())?;
    }
    Ok(())
}

fn permission_level_name(level: PermissionLevel) -> &'static str {
    match level {
        PermissionLevel::Deny => "deny",
        PermissionLevel::Ask => "ask",
        PermissionLevel::AllowAll => "allow-all",
    }
}

fn format_usage(usage: ApiUsage) -> String {
    fn value(value: Option<u64>) -> String {
        value.map_or_else(|| "-".to_string(), |count| count.to_string())
    }
    let rate = match (usage.input_tokens, usage.cache_read_tokens) {
        (Some(input), Some(cache_read)) if input > 0 => {
            format!("{:.2}%", cache_read as f64 / input as f64 * 100.0)
        }
        _ => "-".to_string(),
    };

    format!(
        "input={} output={} cache_read={} cache_write={} rate={}",
        value(usage.input_tokens),
        value(usage.output_tokens),
        value(usage.cache_read_tokens),
        value(usage.cache_write_tokens),
        rate,
    )
}

fn accumulate_usage(total: &mut ApiUsage, usage: ApiUsage) {
    fn accumulate(total: &mut Option<u64>, value: Option<u64>) {
        if let Some(value) = value {
            *total = Some(total.unwrap_or(0).saturating_add(value));
        }
    }

    accumulate(&mut total.input_tokens, usage.input_tokens);
    accumulate(&mut total.output_tokens, usage.output_tokens);
    accumulate(&mut total.cache_read_tokens, usage.cache_read_tokens);
    accumulate(&mut total.cache_write_tokens, usage.cache_write_tokens);
}

fn has_usage(usage: ApiUsage) -> bool {
    usage.input_tokens.is_some()
        || usage.output_tokens.is_some()
        || usage.cache_read_tokens.is_some()
        || usage.cache_write_tokens.is_some()
}

fn show_prompt(editor: &ChatLineEditor, prompt_active: &mut bool) -> Result<()> {
    if !*prompt_active {
        editor.show_prompt()?;
        *prompt_active = true;
    }
    Ok(())
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
            .with_context_group_keys("run", ["system", "session", "turn", "agent", "iteration"]),
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
    let system = HostAgentSystem::new::<StdThread, TokioExecutor>(persistence)?;
    system.link_api(llm_config, claw_agent::ApiUsage::RootAgent, true)?;
    system.start_all()?;
    let session = system.new_session(SessionPersistence::Persistent);
    let (control, events) = system.open_session(session)?;
    let mut chat = ChatDriver::new(control, events);

    eprintln!("Memory:  {MEMORY_DIR}");
    eprintln!("Type a message, or / for commands. Empty line or Ctrl-D to quit.\n");

    let mut editor = ChatLineEditor::new()?;
    let mut turn_active = false;
    let mut prompt_active = false;
    show_prompt(&editor, &mut prompt_active)?;

    loop {
        let activity = if turn_active {
            IdleActivity::Session(chat.events.next().await)
        } else {
            next_idle_activity(editor.next_input(), chat.events.next()).await
        };
        match activity {
            IdleActivity::Input(Some(LineInput::Line(line))) => {
                prompt_active = false;
                let input = line.trim();
                if input.is_empty() {
                    break;
                }
                match parse_input(input) {
                    Ok(CliInput::Message(message)) => {
                        turn_active = chat.submit(message).await;
                        if !turn_active {
                            show_prompt(&editor, &mut prompt_active)?;
                        }
                    }
                    Ok(CliInput::SetPermission(level)) => {
                        chat.set_permission_level(level).await;
                        show_prompt(&editor, &mut prompt_active)?;
                    }
                    Err(error) => {
                        print_event("error", &error.to_string(), EventStyle::Error);
                        show_prompt(&editor, &mut prompt_active)?;
                    }
                }
            }
            IdleActivity::Input(Some(LineInput::Interrupted)) => {
                prompt_active = false;
                show_prompt(&editor, &mut prompt_active)?;
            }
            IdleActivity::Input(Some(LineInput::Eof) | None) => break,
            IdleActivity::Input(Some(LineInput::Failed(error))) => return Err(error.into()),
            IdleActivity::Session(Some(event)) => {
                match chat.render(event, &mut editor, prompt_active)? {
                    RenderOutcome::Continue => {}
                    RenderOutcome::TurnStarted => turn_active = true,
                    RenderOutcome::TurnEnded { user, saw_output } => {
                        turn_active = false;
                        if user && !saw_output {
                            println!("\n(no reply)\n");
                        }
                        show_prompt(&editor, &mut prompt_active)?;
                    }
                    RenderOutcome::Closed => break,
                }
            }
            IdleActivity::Session(None) => break,
        }
    }

    if let Some(usage) = chat.total_usage() {
        if prompt_active {
            editor.print(format_event(
                "total",
                &format_usage(usage),
                EventStyle::Usage,
            ))?;
        } else {
            eprintln!("\n");
            print_event("total", &format_usage(usage), EventStyle::Usage);
        }
    }
    if prompt_active {
        editor.print("Goodbye.".to_string())?;
    } else {
        eprintln!("Goodbye.");
    }
    Ok(())
}

/// Read a required, non-empty environment variable or fail with a clear message.
fn required(key: &str) -> Result<String> {
    match std::env::var(key) {
        Ok(value) if !value.is_empty() => Ok(value),
        _ => bail!("{key} must be set (in env or claw-core/.env.local)"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use claw_api::ApiUsage;

    #[test]
    fn usage_line_includes_provider_cache_counters() {
        let usage = ApiUsage {
            input_tokens: Some(128),
            output_tokens: Some(9),
            cache_read_tokens: Some(96),
            cache_write_tokens: None,
        };

        assert_eq!(
            format_usage(usage),
            "input=128 output=9 cache_read=96 cache_write=- rate=75.00%"
        );
    }

    #[test]
    fn usage_line_omits_rate_when_input_is_unavailable() {
        let usage = ApiUsage {
            input_tokens: None,
            output_tokens: Some(9),
            cache_read_tokens: Some(96),
            cache_write_tokens: None,
        };

        assert_eq!(
            format_usage(usage),
            "input=- output=9 cache_read=96 cache_write=- rate=-"
        );
    }

    #[test]
    fn usage_totals_sum_iterations_and_recompute_rate() {
        let mut total = ApiUsage::default();
        accumulate_usage(
            &mut total,
            ApiUsage {
                input_tokens: Some(100),
                output_tokens: Some(10),
                cache_read_tokens: Some(80),
                cache_write_tokens: None,
            },
        );
        accumulate_usage(
            &mut total,
            ApiUsage {
                input_tokens: Some(300),
                output_tokens: Some(20),
                cache_read_tokens: Some(120),
                cache_write_tokens: Some(50),
            },
        );

        assert_eq!(
            format_usage(total),
            "input=400 output=30 cache_read=200 cache_write=50 rate=50.00%"
        );
        assert!(has_usage(total));
        assert!(!has_usage(ApiUsage::default()));
    }

    #[test]
    fn idle_repl_receives_session_events_without_waiting_for_stdin() {
        let event = SessionEvent::TurnStarted {
            turn: claw_agent::TurnId(7),
            origin: TurnOrigin::Subagent {
                agent: claw_agent::AgentId(3),
            },
        };

        let activity = futures_lite::future::block_on(next_idle_activity(
            std::future::pending(),
            std::future::ready(Some(event.clone())),
        ));

        assert!(matches!(
            activity,
            IdleActivity::Session(Some(received)) if received == event
        ));
    }
}
