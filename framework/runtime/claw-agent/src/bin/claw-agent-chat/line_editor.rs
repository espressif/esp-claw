use std::borrow::Cow;
use std::sync::mpsc::{self, Sender};
use std::thread;

use anstyle::{AnsiColor, Style};
use anyhow::{anyhow, Result};
use rustyline::completion::{Completer, Pair};
use rustyline::error::ReadlineError;
use rustyline::highlight::Highlighter;
use rustyline::hint::{Hint, Hinter};
use rustyline::history::DefaultHistory;
use rustyline::validate::Validator;
use rustyline::{Config, Context, Editor, ExternalPrinter, Helper};
use tokio::sync::mpsc::{unbounded_channel, UnboundedReceiver};

use super::command::command_hint;

pub(super) enum LineInput {
    Line(String),
    Interrupted,
    Eof,
    Failed(ReadlineError),
}

/// Bridges rustyline's blocking editor into the async session event loop.
pub(super) struct ChatLineEditor {
    prompt: Sender<()>,
    input: UnboundedReceiver<LineInput>,
    printer: Box<dyn ExternalPrinter + Send>,
}

impl ChatLineEditor {
    pub(super) fn new() -> Result<Self> {
        let config = Config::builder().auto_add_history(true).build();
        let mut editor = Editor::<CommandHelper, DefaultHistory>::with_config(config)?;
        editor.set_helper(Some(CommandHelper));
        let printer = Box::new(editor.create_external_printer()?);
        let (prompt_tx, prompt_rx) = mpsc::channel();
        let (input_tx, input_rx) = unbounded_channel();

        // One thread owns the editor for its entire lifetime. The async loop
        // sends one token whenever the session is ready for another input.
        thread::Builder::new()
            .name("claw-agent-chat-input".to_string())
            .spawn(move || {
                while prompt_rx.recv().is_ok() {
                    let (input, terminal) = match editor.readline("> ") {
                        Ok(line) => (LineInput::Line(line), false),
                        Err(ReadlineError::Interrupted) => (LineInput::Interrupted, false),
                        Err(ReadlineError::Eof) => (LineInput::Eof, true),
                        Err(error) => (LineInput::Failed(error), true),
                    };
                    if input_tx.send(input).is_err() || terminal {
                        break;
                    }
                }
            })?;

        Ok(Self {
            prompt: prompt_tx,
            input: input_rx,
            printer,
        })
    }

    pub(super) fn show_prompt(&self) -> Result<()> {
        self.prompt
            .send(())
            .map_err(|_| anyhow!("line editor stopped"))
    }

    pub(super) async fn next_input(&mut self) -> Option<LineInput> {
        self.input.recv().await
    }

    pub(super) fn print(&mut self, mut message: String) -> Result<()> {
        if !message.ends_with('\n') {
            message.push('\n');
        }
        self.printer.print(message)?;
        Ok(())
    }
}

struct CommandHint(String);

impl Hint for CommandHint {
    fn display(&self) -> &str {
        &self.0
    }

    fn completion(&self) -> Option<&str> {
        None
    }
}

struct CommandHelper;

impl Completer for CommandHelper {
    type Candidate = Pair;
}

impl Hinter for CommandHelper {
    type Hint = CommandHint;

    fn hint(&self, line: &str, cursor: usize, _context: &Context<'_>) -> Option<Self::Hint> {
        command_hint(line, cursor).map(CommandHint)
    }
}

impl Highlighter for CommandHelper {
    fn highlight_hint<'hint>(&self, hint: &'hint str) -> Cow<'hint, str> {
        let gray = Style::new().fg_color(Some(AnsiColor::BrightBlack.into()));
        Cow::Owned(format!("{gray}{hint}{gray:#}"))
    }
}

impl Validator for CommandHelper {}

impl Helper for CommandHelper {}

#[cfg(test)]
mod tests {
    use rustyline::highlight::Highlighter;

    use super::*;

    #[test]
    fn command_hint_is_rendered_in_gray() {
        let rendered = CommandHelper.highlight_hint("permissions <deny|ask|allow-all>");

        assert!(rendered.starts_with("\u{1b}[90m"));
        assert!(rendered.ends_with("\u{1b}[0m"));
    }
}
