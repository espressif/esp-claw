//! Reusable debug console framework.
//!
//! This crate owns the framework-level console loop, command registration, and
//! command dispatch. Applications provide concrete commands and concrete byte
//! streams.

use std::string::FromUtf8Error;

use thiserror::Error;

pub mod stream;

/// Result type used by the debug console framework.
pub type Result<T> = std::result::Result<T, Error>;

const CONSOLE_PROMPT: &str = "console> ";

/// Error type used by the debug console framework.
#[derive(Debug, Error)]
pub enum Error {
    /// The command line did not contain a command.
    #[error("empty command line")]
    EmptyLine,
    /// A command name is empty or contains whitespace.
    #[error("invalid command name: {0}")]
    InvalidCommandName(&'static str),
    /// Two commands in the same command list have the same name.
    #[error("duplicate command name: {0}")]
    DuplicateCommandName(&'static str),
    /// A matched command node has no handler.
    #[error("missing command handler: {0}")]
    MissingHandler(&'static str),
    /// A command node has children but no selected subcommand.
    #[error("missing subcommand for command: {0}")]
    MissingSubcommand(&'static str),
    /// No registered command matched the first command token.
    #[error("unknown command: {0}")]
    UnknownCommand(String),
    /// The input line ended while a quoted argument was open.
    #[error("unterminated quoted argument")]
    UnterminatedQuote,
    /// The input line ended immediately after an escape character.
    #[error("trailing escape character")]
    TrailingEscape,
    /// A line read from the byte stream was not valid UTF-8.
    #[error("invalid utf-8 command line: {0}")]
    InvalidUtf8(#[from] FromUtf8Error),
    /// A byte stream returned more bytes than the supplied read buffer can hold.
    #[error("byte stream returned invalid read count: {0}")]
    InvalidReadCount(usize),
    /// A byte stream reported an impossible write length.
    #[error("byte stream wrote invalid byte count: {written} of {requested}")]
    InvalidWriteCount {
        /// Number of bytes reported as written.
        written: usize,
        /// Number of bytes requested for the write.
        requested: usize,
    },
    /// A byte stream wrote zero bytes while data remained.
    #[error("byte stream wrote zero bytes")]
    WriteZero,
    /// A byte stream implementation reported an error.
    #[error("byte stream error: {0}")]
    Stream(String),
    /// Internal state became inconsistent.
    #[error("internal console error: {0}")]
    Internal(&'static str),
}

/// Byte-oriented console transport.
pub trait ByteStream {
    /// Reads bytes into `bytes`.
    ///
    /// Returning `Ok(0)` means the stream is closed.
    fn read(&mut self, bytes: &mut [u8]) -> Result<usize>;

    /// Writes bytes from `bytes`.
    ///
    /// Implementations may perform partial writes. The console runtime will
    /// continue writing until the full buffer is sent.
    fn write(&mut self, bytes: &[u8]) -> Result<usize>;

    /// Flushes buffered output.
    fn flush(&mut self) -> Result<()>;
}

/// Command handler function.
pub type CommandFn = fn(CommandContext<'_>, CommandArgs<'_>) -> Result<String>;

/// Static command metadata.
#[derive(Debug)]
pub struct CommandSpec {
    /// Command name for this tree node.
    pub name: &'static str,
    /// Help text shown by help-style commands.
    pub help: &'static str,
    /// Usage text for this command.
    pub usage: Option<&'static str>,
    /// Handler for this command node.
    pub handler: Option<CommandFn>,
    /// Static subcommands.
    pub children: &'static [CommandSpec],
}

/// Context passed to a command handler.
#[derive(Clone, Copy, Debug)]
pub struct CommandContext<'a> {
    commands: &'a [&'static CommandSpec],
}

impl<'a> CommandContext<'a> {
    /// Returns registered top-level command specs.
    #[must_use]
    pub fn commands(&self) -> &'a [&'static CommandSpec] {
        self.commands
    }
}

/// Parsed command arguments passed to a command handler.
#[derive(Clone, Copy, Debug)]
pub struct CommandArgs<'a> {
    /// Original command line.
    pub line: &'a str,
    /// Matched command path, including the top-level command.
    pub command_path: &'a [&'a str],
    /// Remaining arguments after the matched command path.
    pub argv: &'a [&'a str],
}

/// Console runtime marker returned after a stream closes.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ConsoleRuntime;

/// Debug console framework.
#[derive(Debug)]
pub struct DebugConsole<S: ByteStream + Default> {
    commands: Vec<&'static CommandSpec>,
    stream: S,
}

impl<S: ByteStream + Default> DebugConsole<S> {
    /// Creates a debug console over the default instance of one byte stream type.
    #[must_use]
    pub fn new() -> Self {
        Self {
            commands: Vec::new(),
            stream: S::default(),
        }
    }

    /// Registers one static command tree.
    ///
    /// # Errors
    ///
    /// Returns an error if the command tree is invalid or duplicates an
    /// existing top-level command.
    pub fn register(&mut self, command: &'static CommandSpec) -> Result<()> {
        validate_command_tree(command)?;
        reject_duplicate_name(&self.commands, command.name)?;
        self.commands.push(command);
        Ok(())
    }

    /// Starts the console loop.
    ///
    /// The loop returns when the byte stream returns `Ok(0)` from `read`.
    ///
    /// # Errors
    ///
    /// Returns an error if reading from or writing to the byte stream fails.
    pub fn start(&mut self) -> Result<ConsoleRuntime> {
        let mut reader = LineReader::default();

        loop {
            write_all(&mut self.stream, CONSOLE_PROMPT.as_bytes())?;

            let Some(line) = reader.read_line(&mut self.stream)? else {
                return Ok(ConsoleRuntime);
            };

            match self.dispatch_line(&line) {
                Ok(output) => {
                    write_all(&mut self.stream, output.as_bytes())?;
                }
                Err(error) => {
                    write_error(&mut self.stream, &error)?;
                }
            }
        }
    }

    fn dispatch_line(&self, line: &str) -> Result<String> {
        let tokens = parse_line(line)?;
        let argv = tokens.iter().map(String::as_str).collect::<Vec<_>>();
        let matched = find_command(&self.commands, &argv)?;
        let Some(handler) = matched.command.handler else {
            return Err(Error::MissingHandler(matched.command.name));
        };

        handler(
            CommandContext {
                commands: &self.commands,
            },
            CommandArgs {
                line,
                command_path: matched.path,
                argv: matched.argv,
            },
        )
    }
}

impl<S: ByteStream + Default> Default for DebugConsole<S> {
    fn default() -> Self {
        Self::new()
    }
}

fn validate_command_tree(command: &'static CommandSpec) -> Result<()> {
    validate_command_name(command.name)?;

    validate_unique_child_names(command.children)?;
    for child in command.children {
        validate_command_tree(child)?;
    }

    Ok(())
}

fn validate_command_name(name: &'static str) -> Result<()> {
    if name.is_empty() || name.chars().any(char::is_whitespace) {
        return Err(Error::InvalidCommandName(name));
    }
    Ok(())
}

fn validate_unique_child_names(commands: &'static [CommandSpec]) -> Result<()> {
    let mut seen = Vec::new();
    for command in commands {
        reject_duplicate_name(&seen, command.name)?;
        seen.push(command);
    }
    Ok(())
}

fn reject_duplicate_name(
    commands: &[&'static CommandSpec],
    candidate_name: &'static str,
) -> Result<()> {
    if commands
        .iter()
        .any(|command| command.name == candidate_name)
    {
        return Err(Error::DuplicateCommandName(candidate_name));
    }
    Ok(())
}

#[derive(Clone, Copy, Debug)]
struct CommandMatch<'a> {
    command: &'static CommandSpec,
    path: &'a [&'a str],
    argv: &'a [&'a str],
}

fn find_command<'a>(
    commands: &[&'static CommandSpec],
    argv: &'a [&'a str],
) -> Result<CommandMatch<'a>> {
    let Some(first) = argv.first() else {
        return Err(Error::EmptyLine);
    };

    for command in commands {
        if command.name == *first {
            return match_longest(command, argv, 1);
        }
    }

    Err(Error::UnknownCommand((*first).to_owned()))
}

fn match_longest<'a>(
    command: &'static CommandSpec,
    argv: &'a [&'a str],
    depth: usize,
) -> Result<CommandMatch<'a>> {
    if let Some(next) = argv.get(depth) {
        for child in command.children {
            if child.name == *next {
                let next_depth = depth
                    .checked_add(1)
                    .ok_or(Error::Internal("command depth overflow"))?;
                return match_longest(child, argv, next_depth);
            }
        }
    }

    if command.handler.is_none() && !command.children.is_empty() {
        return Err(Error::MissingSubcommand(command.name));
    }

    let path = argv
        .get(..depth)
        .ok_or(Error::Internal("invalid command path"))?;
    let remaining = argv
        .get(depth..)
        .ok_or(Error::Internal("invalid command argv"))?;

    Ok(CommandMatch {
        command,
        path,
        argv: remaining,
    })
}

fn parse_line(line: &str) -> Result<Vec<String>> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut in_quote = false;
    let mut escaped = false;
    let mut token_started = false;

    for character in line.chars() {
        if escaped {
            current.push(character);
            escaped = false;
            token_started = true;
            continue;
        }

        match character {
            '\\' => {
                escaped = true;
                token_started = true;
            }
            '"' => {
                in_quote = !in_quote;
                token_started = true;
            }
            character if character.is_whitespace() && !in_quote => {
                if token_started {
                    tokens.push(current);
                    current = String::new();
                    token_started = false;
                }
            }
            character => {
                current.push(character);
                token_started = true;
            }
        }
    }

    if escaped {
        return Err(Error::TrailingEscape);
    }
    if in_quote {
        return Err(Error::UnterminatedQuote);
    }
    if token_started {
        tokens.push(current);
    }

    Ok(tokens)
}

#[derive(Debug, Default)]
struct LineReader {
    skip_next_lf: bool,
}

impl LineReader {
    fn read_line<S: ByteStream>(&mut self, stream: &mut S) -> Result<Option<String>> {
        let mut line = Vec::new();
        let mut byte = [0_u8; 1];

        loop {
            let read = stream.read(&mut byte)?;
            match read {
                0 => {
                    if line.is_empty() {
                        return Ok(None);
                    }
                    return String::from_utf8(line)
                        .map(Some)
                        .map_err(Error::InvalidUtf8);
                }
                1 => {
                    let Some(value) = byte.first().copied() else {
                        return Err(Error::Internal("missing read byte"));
                    };

                    if self.skip_next_lf {
                        self.skip_next_lf = false;
                        if value == b'\n' {
                            continue;
                        }
                    }

                    match value {
                        b'\n' => {
                            return String::from_utf8(line)
                                .map(Some)
                                .map_err(Error::InvalidUtf8);
                        }
                        b'\r' => {
                            self.skip_next_lf = true;
                            return String::from_utf8(line)
                                .map(Some)
                                .map_err(Error::InvalidUtf8);
                        }
                        8 | 127 => {
                            line.pop();
                        }
                        value => line.push(value),
                    }
                }
                count => return Err(Error::InvalidReadCount(count)),
            }
        }
    }
}

fn write_all<S: ByteStream>(stream: &mut S, mut bytes: &[u8]) -> Result<()> {
    while !bytes.is_empty() {
        let written = stream.write(bytes)?;
        if written == 0 {
            return Err(Error::WriteZero);
        }

        bytes = bytes.get(written..).ok_or(Error::InvalidWriteCount {
            written,
            requested: bytes.len(),
        })?;
    }

    stream.flush()
}

fn write_error<S: ByteStream>(stream: &mut S, error: &Error) -> Result<()> {
    let message = format!("error: {error}\n");
    write_all(stream, message.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug, Default)]
    struct MemoryStream {
        input: Vec<u8>,
        output: Vec<u8>,
    }

    impl MemoryStream {
        fn with_input(input: &[u8]) -> Self {
            Self {
                input: input.to_vec(),
                output: Vec::new(),
            }
        }
    }

    impl ByteStream for MemoryStream {
        fn read(&mut self, bytes: &mut [u8]) -> Result<usize> {
            let Some(slot) = bytes.first_mut() else {
                return Ok(0);
            };
            let Some(byte) = self.input.first().copied() else {
                return Ok(0);
            };
            self.input.remove(0);
            *slot = byte;
            Ok(1)
        }

        fn write(&mut self, bytes: &[u8]) -> Result<usize> {
            self.output.extend_from_slice(bytes);
            Ok(bytes.len())
        }

        fn flush(&mut self) -> Result<()> {
            Ok(())
        }
    }

    fn ping(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
        Ok(args.argv.join(" "))
    }

    static PING: CommandSpec = CommandSpec {
        name: "ping",
        help: "Ping",
        usage: Some("ping <text>"),
        handler: Some(ping),
        children: &[],
    };

    static UNWIRED: CommandSpec = CommandSpec {
        name: "unwired",
        help: "Unwired",
        usage: Some("unwired"),
        handler: None,
        children: &[],
    };

    #[test]
    fn dispatches_registered_command() -> Result<()> {
        let mut console = DebugConsole {
            commands: Vec::new(),
            stream: MemoryStream::with_input(b"ping hello\n"),
        };
        console.register(&PING)?;
        console.start()?;

        let output = String::from_utf8(console.stream.output).map_err(Error::InvalidUtf8)?;
        assert_eq!(output, "console> helloconsole> ");
        Ok(())
    }

    #[test]
    fn parses_quoted_argument() -> Result<()> {
        let mut console = DebugConsole {
            commands: Vec::new(),
            stream: MemoryStream::with_input(b"ping \"hello world\"\n"),
        };
        console.register(&PING)?;
        console.start()?;

        let output = String::from_utf8(console.stream.output).map_err(Error::InvalidUtf8)?;
        assert_eq!(output, "console> hello worldconsole> ");
        Ok(())
    }

    #[test]
    fn allows_unwired_leaf_command() -> Result<()> {
        let mut console = DebugConsole {
            commands: Vec::new(),
            stream: MemoryStream::with_input(b"unwired\n"),
        };
        console.register(&UNWIRED)?;
        console.start()?;

        let output = String::from_utf8(console.stream.output).map_err(Error::InvalidUtf8)?;
        assert_eq!(
            output,
            "console> error: missing command handler: unwired\nconsole> "
        );
        Ok(())
    }
}
