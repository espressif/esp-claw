# Debug Console

```rust
struct DebugConsole<S: ByteStream + Default> {
    commands: Vec<&'static CommandSpec>,
    stream: S,

    pub fn new() -> DebugConsole<S>
    pub fn register(&mut self, command: &'static CommandSpec) -> Result<()>
    pub fn start(&mut self) -> Result<ConsoleRuntime>
}

trait ByteStream {
    fn read(&mut self, bytes: &mut [u8]) -> Result<usize>
    fn write(&mut self, bytes: &[u8]) -> Result<usize>
    fn flush(&mut self) -> Result<()>
}

struct CommandSpec {
    name: &'static str,
    help: &'static str,
    usage: Option<&'static str>,
    handler: Option<CommandFn>,
    children: &'static [CommandSpec],
}

type CommandFn = fn(CommandContext<'_>, CommandArgs<'_>) -> Result<String>;

struct CommandContext<'a> {
    commands: &'a [&'static CommandSpec],
}

struct CommandArgs<'a> {
    line: &'a str,
    command_path: &'a [&'a str],
    argv: &'a [&'a str],
}

```

## Rules

```rust
// debug-console is framework debug infrastructure.
framework/services/debug-console // owns reusable console runtime and capture
applications/shared/app_claw // owns app_claw command wiring
applications/edge_agent // owns app-local commands such as wifi

// debug-console is not a capability.
Capability::Tool(debug_console) // forbidden
CLAW_CAP_FLAG_CALLABLE_BY_LLM // forbidden here

// The service must not know app semantics.
ask/session/cap/auto // registered by app_claw, not implemented here
wifi // registered by edge_agent, not implemented here

// Static registration is the default.
static CommandSpec // preferred
static child CommandSpec // preferred for subcommands
console> prompt // fixed by debug-console
DebugConsole.commands: Vec<&'static CommandSpec> // allowed
DebugConsole.stream: S // owns one concrete ByteStream
heap-allocated command metadata // forbidden unless runtime extension requires it
Box<dyn CommandHandler> // forbidden for normal firmware commands
String command name/help/usage // forbidden for normal firmware commands
String prompt // forbidden for normal firmware builds

// No artificial limits in the public API.
max_args in public config // forbidden
task_stack_size in DebugConsole // forbidden

// ByteStream is the IO boundary.
DebugConsole<S: ByteStream + Default> // static dispatch, preferred
start(&mut self) // preferred
start<S: ByteStream>(stream) // forbidden
Box<dyn ByteStream> // forbidden for normal firmware path
&mut dyn ByteStream // forbidden for normal firmware path
stream::Stdio implements ByteStream + Default // stdio backend
stream::EspIdf implements ByteStream + Default // espidf backend
CONFIG_ESP_CONSOLE_UART_DEFAULT/CUSTOM // EspIdf selects UART backend
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG // EspIdf selects USB Serial/JTAG backend
CONFIG_ESP_CONSOLE_USB_CDC // EspIdf selects USB CDC backend

// Command execution is internal to start().
dispatch_line(line) -> Result<String> // private command executor
public run(line, output) // forbidden
scrape ESP_LOG output // forbidden for command assertions

// ESP console is not the dispatch layer.
esp_console_cmd_register // forbidden
esp_console_run // forbidden
esp_console_new_repl_uart // reference behavior only
esp_console_new_repl_usb_serial_jtag // reference behavior only
esp_console_new_repl_usb_cdc // reference behavior only
ByteStream line reader -> dispatch_line // preferred

// Agent access to CLI is a separate allow-listed bridge.
run_cli_command // belongs to an optional capability depending on debug-console
```

## Static Registration

```rust
static APP_CLAW_COMMANDS: &[CommandSpec] = &[
    CommandSpec { name: "ask", help: "Submit a prompt using the current session", usage: Some("ask <prompt>"), handler: Some(app_claw_cmd_ask), children: &[] },
    CommandSpec { name: "session", help: "Show or switch the current session", usage: Some("session [id]"), handler: Some(app_claw_cmd_session), children: &[] },
    CommandSpec { name: "cap", help: "Capability registry operations", usage: Some("cap <subcommand> ..."), handler: None, children: CAP_COMMANDS },
    CommandSpec { name: "auto", help: "Automation router operations", usage: Some("auto <subcommand> ..."), handler: None, children: AUTO_COMMANDS },
    CommandSpec { name: "lua", help: "Lua script operations", usage: Some("lua <options>"), handler: Some(cap_lua_cmd), children: &[] },
    CommandSpec { name: "scheduler", help: "Scheduler operations", usage: Some("scheduler <options>"), handler: Some(cap_scheduler_cmd), children: &[] },
    CommandSpec { name: "event_router", help: "Event router operations", usage: Some("event_router <options>"), handler: Some(cap_router_mgr_cmd), children: &[] },
    CommandSpec { name: "time", help: "Time operations", usage: Some("time --now"), handler: Some(cap_time_cmd), children: &[] },
];

static EDGE_AGENT_COMMANDS: &[CommandSpec] = &[
    CommandSpec { name: "wifi", help: "Wi-Fi operations", usage: Some("wifi <options>"), handler: Some(edge_agent_cmd_wifi), children: &[] },
];

static CAP_COMMANDS: &[CommandSpec] = &[
    CommandSpec { name: "list", help: "List capabilities", usage: Some("cap list"), handler: Some(app_claw_cmd_cap_list), children: &[] },
    CommandSpec { name: "call", help: "Call one capability", usage: Some("cap call <name> <json>"), handler: Some(app_claw_cmd_cap_call), children: &[] },
    CommandSpec { name: "groups", help: "List capability groups", usage: Some("cap groups"), handler: Some(app_claw_cmd_cap_groups), children: &[] },
    CommandSpec { name: "enable", help: "Enable a capability group", usage: Some("cap enable <group_id>"), handler: Some(app_claw_cmd_cap_enable), children: &[] },
    CommandSpec { name: "disable", help: "Disable a capability group", usage: Some("cap disable <group_id>"), handler: Some(app_claw_cmd_cap_disable), children: &[] },
    CommandSpec { name: "unload", help: "Unload a capability group", usage: Some("cap unload <group_id>"), handler: Some(app_claw_cmd_cap_unload), children: &[] },
    CommandSpec { name: "load", help: "Load a debug capability group", usage: Some("cap load <plugin>"), handler: Some(app_claw_cmd_cap_load), children: &[] },
];

static AUTO_COMMANDS: &[CommandSpec] = &[
    CommandSpec { name: "reload", help: "Reload router rules", usage: Some("auto reload"), handler: Some(app_claw_cmd_auto_reload), children: &[] },
    CommandSpec { name: "rules", help: "List router rules", usage: Some("auto rules"), handler: Some(app_claw_cmd_auto_rules), children: &[] },
    CommandSpec { name: "rule", help: "Get one router rule", usage: Some("auto rule <id>"), handler: Some(app_claw_cmd_auto_rule), children: &[] },
    CommandSpec { name: "add_rule", help: "Add one router rule", usage: Some("auto add_rule <json>"), handler: Some(app_claw_cmd_auto_add_rule), children: &[] },
    CommandSpec { name: "update_rule", help: "Update one router rule", usage: Some("auto update_rule <json>"), handler: Some(app_claw_cmd_auto_update_rule), children: &[] },
    CommandSpec { name: "delete_rule", help: "Delete one router rule", usage: Some("auto delete_rule <id>"), handler: Some(app_claw_cmd_auto_delete_rule), children: &[] },
    CommandSpec { name: "last", help: "Show last router result", usage: Some("auto last"), handler: Some(app_claw_cmd_auto_last), children: &[] },
    CommandSpec { name: "emit_message", help: "Publish a message event", usage: Some("auto emit_message <source_cap> <channel> <chat_id> <text...>"), handler: Some(app_claw_cmd_auto_emit_message), children: &[] },
    CommandSpec { name: "emit_trigger", help: "Publish a trigger event", usage: Some("auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>"), handler: Some(app_claw_cmd_auto_emit_trigger), children: &[] },
];
```

## Construction

```rust
const CONSOLE_PROMPT: &str = "console> ";

fn new<S: ByteStream + Default>() -> DebugConsole<S> {
    DebugConsole {
        commands: Vec::new(),
        stream: S::default(),
    }
}
```

## Register Flow

```rust
fn register(&mut self, command: &'static CommandSpec) -> Result<()> {
    validate_command_tree(command)?
    reject_duplicate_top_level_name(&self.commands, command.name)?
    self.commands.push(command)
    Ok(())
}

fn app_claw_register_debug_commands<S: ByteStream + Default>(console: &mut DebugConsole<S>) -> Result<()> {
    for command in APP_CLAW_COMMANDS {
        console.register(command)?
    }
    Ok(())
}

fn edge_agent_register_debug_commands<S: ByteStream + Default>(console: &mut DebugConsole<S>) -> Result<()> {
    for command in EDGE_AGENT_COMMANDS {
        console.register(command)?
    }
    Ok(())
}
```

## Command Lookup

```rust
struct CommandMatch<'a> {
    command: &'static CommandSpec,
    path: &'a [&'a str],
    argv: &'a [&'a str],
}

fn find_command(commands: &[&'static CommandSpec], argv: &[&str]) -> Result<CommandMatch<'_>> {
    let Some(first) = argv.first() else {
        return Err(Error::EmptyLine)
    };

    for &command in commands {
        if command.name == *first {
            return match_longest(command, argv, 1)
        }
    }
    Err(Error::UnknownCommand(first))
}

fn match_longest(command: &'static CommandSpec, argv: &[&str], depth: usize) -> Result<CommandMatch<'_>> {
    if let Some(next) = argv.get(depth) {
        for child in command.children {
            if child.name == *next {
                return match_longest(child, argv, depth + 1)
            }
        }
    }

    if command.handler.is_none() && !command.children.is_empty() {
        return Err(Error::MissingSubcommand(command.name))
    }

    Ok(CommandMatch {
        command,
        path: &argv[..depth],
        argv: &argv[depth..],
    })
}
```

## App Construction

```rust
fn app_start_debug_console() -> Result<()> {
    let mut console = DebugConsole::<debug_console::stream::EspIdf>::new()

    app_claw_register_debug_commands(&mut console)?
    edge_agent_register_debug_commands(&mut console)?
    console.start()
}
```

## Dispatch Flow

```rust
fn dispatch_line(&self, line: &str) -> Result<String> {
    let parsed = parse_line(line)?
    let matched = find_command(&self.commands, parsed.argv)?
    let Some(handler) = matched.command.handler else {
        return Err(Error::MissingHandler(matched.command.name))
    }

    handler(
        CommandContext { commands: &self.commands },
        CommandArgs { line, command_path: matched.path, argv: matched.argv },
    )
}
```

## Start Flow

```rust
fn start(&mut self) -> Result<ConsoleRuntime> {
    let mut reader = ByteStreamLineReader::default()
    let mut line = [0_u8; 512]

    write_all(&mut self.stream, CONSOLE_PROMPT.as_bytes())?
    while let Some(line) = reader.read_line(&mut self.stream, &mut line)? {
        match self.dispatch_line(line) {
            Ok(output) => write_all(&mut self.stream, output.as_bytes())?,
            Err(err) => print_error(&mut self.stream, err)?,
        }
        write_all(&mut self.stream, CONSOLE_PROMPT.as_bytes())?
    }
    Ok(ConsoleRuntime)
}

struct Stdio;
impl ByteStream for Stdio;
impl Default for Stdio;

struct EspIdf;
impl ByteStream for EspIdf;
impl Default for EspIdf;
```

## Open

```rust
// App state ownership.
CommandContext // should app_claw attach a static context pointer, or use module statics?

// Dynamic extension.
register_dynamic // needed for plugin/debug modules, or forbid for now?

// Byte stream frontend.
ByteStreamLineReader // minimal CR/LF/backspace handling first

// Logs.
Captured logs // include ESP_LOG output, or keep command output only?
```
