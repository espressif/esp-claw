# debug-console

Small Rust framework for a byte-stream debug console.

It owns line reading, command parsing, static command registration, command
lookup, and command dispatch. The caller provides one `ByteStream + Default`
type and registers command specs.

## `main.rs`

```rust
use std::io::{self, Read, Write};

use debug_console::{
    ByteStream, CommandArgs, CommandContext, CommandSpec, DebugConsole, Error, Result,
};

#[derive(Debug, Default)]
struct Stdio;

fn io_error(error: io::Error) -> Error {
    Error::Stream(error.to_string())
}

impl ByteStream for Stdio {
    fn read(&mut self, bytes: &mut [u8]) -> Result<usize> {
        io::stdin().read(bytes).map_err(io_error)
    }

    fn write(&mut self, bytes: &[u8]) -> Result<usize> {
        io::stdout().write(bytes).map_err(io_error)
    }

    fn flush(&mut self) -> Result<()> {
        io::stdout().flush().map_err(io_error)
    }
}

fn hello(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let name = args.argv.first().copied().unwrap_or("world");
    Ok(format!("hello {name}\n"))
}

static HELLO: CommandSpec = CommandSpec {
    name: "hello",
    help: "Print a greeting",
    usage: Some("hello <name>"),
    handler: Some(hello),
    children: &[],
};

fn main() -> Result<()> {
    let mut console = DebugConsole::<Stdio>::new();
    console.register(&HELLO)?;
    console.start()?;
    Ok(())
}
```

The prompt is fixed by the crate:

```text
console>
```

## Subcommands

Use static child specs for nested commands.

```rust
fn status(_context: CommandContext<'_>, _args: CommandArgs<'_>) -> Result<String> {
    Ok("ok".to_owned())
}

fn echo(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    Ok(args.argv.join(" "))
}

static DEBUG_CHILDREN: &[CommandSpec] = &[
    CommandSpec {
        name: "status",
        help: "Show status",
        usage: Some("debug status"),
        handler: Some(status),
        children: &[],
    },
    CommandSpec {
        name: "echo",
        help: "Echo arguments",
        usage: Some("debug echo <text>"),
        handler: Some(echo),
        children: &[],
    },
];

static DEBUG: CommandSpec = CommandSpec {
    name: "debug",
    help: "Debug commands",
    usage: Some("debug <command>"),
    handler: None,
    children: DEBUG_CHILDREN,
};
```

Register `DEBUG`, then `debug status` and `debug echo text` dispatch to their
child handlers.
