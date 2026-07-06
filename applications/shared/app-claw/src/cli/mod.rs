//! app-claw debug console commands.

use debug_console::{
    ByteStream, CommandArgs, CommandContext, CommandSpec, ConsoleRuntime, DebugConsole, Error,
    Result,
};

use crate::sys;

const AGENT_ASK_TIMEOUT_MS: u32 = 300_000;

/// Agent session write behavior for one prompt submission.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AgentSessionMode {
    /// Read and update the named session.
    Persist,
    /// Use the named session boundary without persisting this exchange.
    Volatile,
}

impl AgentSessionMode {
    /// Parses a CLI session mode token.
    #[must_use]
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "persist" => Some(Self::Persist),
            "volatile" => Some(Self::Volatile),
            _ => None,
        }
    }

    /// Returns the stable CLI spelling.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Persist => "persist",
            Self::Volatile => "volatile",
        }
    }
}

/// Registers app-claw debug console commands.
///
/// # Errors
///
/// Returns an error if a command spec is invalid or duplicates an existing
/// console command.
pub fn register<S>(console: &mut DebugConsole<S>) -> Result<()>
where
    S: ByteStream + Default,
{
    for command in COMMANDS {
        console.register(command)?;
    }

    Ok(())
}

/// Starts an app-claw debug console over the default stream type.
///
/// # Errors
///
/// Returns an error if command registration fails or the console stream fails.
pub fn start<S>() -> Result<ConsoleRuntime>
where
    S: ByteStream + Default,
{
    let mut console = DebugConsole::<S>::new();
    register(&mut console)?;
    console.start()
}

fn help(context: CommandContext<'_>, _args: CommandArgs<'_>) -> Result<String> {
    let mut output = String::new();

    for command in context.commands() {
        output.push_str(command.name);
        output.push_str(" - ");
        output.push_str(command.help);
        output.push('\n');
    }

    Ok(output)
}

fn agent_ask(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let mut argv = args.argv.iter().copied();
    let mode = next_arg(&mut argv, "agent ask <mode> <session_id> <prompt>")?;
    let mode = AgentSessionMode::parse(mode)
        .ok_or_else(|| usage_error("agent ask <mode> <session_id> <prompt>"))?;
    let session_id = next_arg(&mut argv, "agent ask <mode> <session_id> <prompt>")?
        .parse::<u32>()
        .map_err(|_| usage_error("agent ask <mode> <session_id> <prompt>"))?;
    if session_id == 0 {
        return Err(Error::Stream("session id must be non-zero".to_owned()));
    }
    let prompt = join_args(argv);
    if prompt.is_empty() {
        return Err(usage_error("agent ask <mode> <session_id> <prompt>"));
    }

    match mode {
        AgentSessionMode::Persist => {
            sys::agent_submit_session(session_id, &prompt, AGENT_ASK_TIMEOUT_MS)
        }
        AgentSessionMode::Volatile => Err(Error::Stream(
            "agent ask volatile is not implemented by claw_agent yet".to_owned(),
        )),
    }
}

fn agent_session_create(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "agent session create")?;
    sys::agent_session_create()
}

fn agent_session_list(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "agent session list")?;
    sys::agent_session_list()
}

fn agent_session_delete(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let session_id = require_one_arg(args, "agent session delete <session_id>")?
        .parse::<u32>()
        .map_err(|_| usage_error("agent session delete <session_id>"))?;
    if session_id == 0 {
        return Err(Error::Stream("session id must be non-zero".to_owned()));
    }
    sys::agent_session_delete(session_id)
}

fn cap_list(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "cap list")?;
    sys::cap_list()
}

fn cap_call(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let mut argv = args.argv.iter().copied();
    let name = next_arg(&mut argv, "cap call <name> <json>")?;
    let input_json = join_args(argv);
    if input_json.is_empty() {
        return Err(usage_error("cap call <name> <json>"));
    }

    sys::cap_call(name, &input_json)
}

fn cap_groups(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "cap groups")?;
    sys::cap_groups()
}

fn cap_enable(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let group_id = require_one_arg(args, "cap enable <group_id>")?;
    sys::cap_enable_group(group_id)
}

fn cap_disable(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let group_id = require_one_arg(args, "cap disable <group_id>")?;
    sys::cap_disable_group(group_id)
}

fn cap_unload(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let group_id = require_one_arg(args, "cap unload <group_id>")?;
    sys::cap_unload_group(group_id)
}

fn auto_reload(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "auto reload")?;
    sys::cap_call("reload_router_rules", "{}")
}

fn auto_rules(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "auto rules")?;
    sys::cap_call("list_router_rules", "{}")
}

fn auto_rule(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let id = require_one_arg(args, "auto rule <id>")?;
    sys::cap_call("get_router_rule", &json_object_string("id", id))
}

fn auto_last(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    require_no_args(args, "auto last")?;
    sys::event_router_last()
}

fn auto_add_rule(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let rule_json = require_joined_args(args, "auto add_rule <json>")?;
    sys::cap_call(
        "add_router_rule",
        &json_object_string("rule_json", &rule_json),
    )
}

fn auto_update_rule(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let rule_json = require_joined_args(args, "auto update_rule <json>")?;
    sys::cap_call(
        "update_router_rule",
        &json_object_string("rule_json", &rule_json),
    )
}

fn auto_delete_rule(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let id = require_one_arg(args, "auto delete_rule <id>")?;
    sys::cap_call("delete_router_rule", &json_object_string("id", id))
}

fn auto_emit_message(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let mut argv = args.argv.iter().copied();
    let source_cap = next_arg(
        &mut argv,
        "auto emit_message <source_cap> <channel> <chat_id> <text>",
    )?;
    let channel = next_arg(
        &mut argv,
        "auto emit_message <source_cap> <channel> <chat_id> <text>",
    )?;
    let chat_id = next_arg(
        &mut argv,
        "auto emit_message <source_cap> <channel> <chat_id> <text>",
    )?;
    let text = join_args(argv);
    if text.is_empty() {
        return Err(usage_error(
            "auto emit_message <source_cap> <channel> <chat_id> <text>",
        ));
    }

    sys::event_router_publish_message(source_cap, channel, chat_id, &text)
}

fn auto_emit_trigger(_context: CommandContext<'_>, args: CommandArgs<'_>) -> Result<String> {
    let mut argv = args.argv.iter().copied();
    let source_cap = next_arg(
        &mut argv,
        "auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>",
    )?;
    let event_type = next_arg(
        &mut argv,
        "auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>",
    )?;
    let event_key = next_arg(
        &mut argv,
        "auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>",
    )?;
    let payload_json = join_args(argv);
    if payload_json.is_empty() {
        return Err(usage_error(
            "auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>",
        ));
    }

    sys::event_router_publish_trigger(source_cap, event_type, event_key, &payload_json)
}

fn require_no_args(args: CommandArgs<'_>, usage: &'static str) -> Result<()> {
    if args.argv.is_empty() {
        Ok(())
    } else {
        Err(usage_error(usage))
    }
}

fn require_one_arg<'a>(args: CommandArgs<'a>, usage: &'static str) -> Result<&'a str> {
    let mut argv = args.argv.iter().copied();
    let value = next_arg(&mut argv, usage)?;
    if argv.next().is_some() {
        return Err(usage_error(usage));
    }
    Ok(value)
}

fn require_joined_args(args: CommandArgs<'_>, usage: &'static str) -> Result<String> {
    let value = join_args(args.argv.iter().copied());
    if value.is_empty() {
        return Err(usage_error(usage));
    }
    Ok(value)
}

fn next_arg<'a>(argv: &mut impl Iterator<Item = &'a str>, usage: &'static str) -> Result<&'a str> {
    argv.next().ok_or_else(|| usage_error(usage))
}

fn join_args<'a>(argv: impl IntoIterator<Item = &'a str>) -> String {
    let mut output = String::new();
    for arg in argv {
        if !output.is_empty() {
            output.push(' ');
        }
        output.push_str(arg);
    }
    output
}

fn usage_error(usage: &'static str) -> Error {
    Error::Stream(format!("usage: {usage}"))
}

fn json_object_string(key: &str, value: &str) -> String {
    let mut output = String::new();
    output.push('{');
    push_json_string(&mut output, key);
    output.push(':');
    push_json_string(&mut output, value);
    output.push('}');
    output
}

fn push_json_string(output: &mut String, value: &str) {
    use std::fmt::Write as _;

    output.push('"');
    for character in value.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            character if character.is_control() => {
                let _ = write!(output, "\\u{:04x}", character as u32);
            }
            character => output.push(character),
        }
    }
    output.push('"');
}

static AGENT_SESSION_COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "create",
        help: "Create an agent session",
        usage: Some("agent session create"),
        handler: Some(agent_session_create),
        children: &[],
    },
    CommandSpec {
        name: "list",
        help: "List agent sessions",
        usage: Some("agent session list"),
        handler: Some(agent_session_list),
        children: &[],
    },
    CommandSpec {
        name: "delete",
        help: "Delete an agent session",
        usage: Some("agent session delete <session_id>"),
        handler: Some(agent_session_delete),
        children: &[],
    },
];

static AGENT_COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "ask",
        help: "Submit a prompt to an explicit session",
        usage: Some("agent ask <mode> <session_id> <prompt>"),
        handler: Some(agent_ask),
        children: &[],
    },
    CommandSpec {
        name: "session",
        help: "Agent session operations",
        usage: Some("agent session <command>"),
        handler: None,
        children: AGENT_SESSION_COMMANDS,
    },
];

static CAP_COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "list",
        help: "List capabilities",
        usage: Some("cap list"),
        handler: Some(cap_list),
        children: &[],
    },
    CommandSpec {
        name: "call",
        help: "Call one capability",
        usage: Some("cap call <name> <json>"),
        handler: Some(cap_call),
        children: &[],
    },
    CommandSpec {
        name: "groups",
        help: "List capability groups",
        usage: Some("cap groups"),
        handler: Some(cap_groups),
        children: &[],
    },
    CommandSpec {
        name: "enable",
        help: "Enable a capability group",
        usage: Some("cap enable <group_id>"),
        handler: Some(cap_enable),
        children: &[],
    },
    CommandSpec {
        name: "disable",
        help: "Disable a capability group",
        usage: Some("cap disable <group_id>"),
        handler: Some(cap_disable),
        children: &[],
    },
    CommandSpec {
        name: "unload",
        help: "Unload a capability group",
        usage: Some("cap unload <group_id>"),
        handler: Some(cap_unload),
        children: &[],
    },
    CommandSpec {
        name: "load",
        help: "Load a capability group",
        usage: Some("cap load <plugin>"),
        handler: None,
        children: &[],
    },
];

static AUTO_COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "reload",
        help: "Reload automation rules",
        usage: Some("auto reload"),
        handler: Some(auto_reload),
        children: &[],
    },
    CommandSpec {
        name: "rules",
        help: "List automation rules",
        usage: Some("auto rules"),
        handler: Some(auto_rules),
        children: &[],
    },
    CommandSpec {
        name: "rule",
        help: "Show one automation rule",
        usage: Some("auto rule <id>"),
        handler: Some(auto_rule),
        children: &[],
    },
    CommandSpec {
        name: "last",
        help: "Show last automation result",
        usage: Some("auto last"),
        handler: Some(auto_last),
        children: &[],
    },
    CommandSpec {
        name: "add_rule",
        help: "Add one automation rule",
        usage: Some("auto add_rule <json>"),
        handler: Some(auto_add_rule),
        children: &[],
    },
    CommandSpec {
        name: "update_rule",
        help: "Update one automation rule",
        usage: Some("auto update_rule <json>"),
        handler: Some(auto_update_rule),
        children: &[],
    },
    CommandSpec {
        name: "delete_rule",
        help: "Delete one automation rule",
        usage: Some("auto delete_rule <id>"),
        handler: Some(auto_delete_rule),
        children: &[],
    },
    CommandSpec {
        name: "emit_message",
        help: "Publish a message event",
        usage: Some("auto emit_message <source_cap> <channel> <chat_id> <text>"),
        handler: Some(auto_emit_message),
        children: &[],
    },
    CommandSpec {
        name: "emit_trigger",
        help: "Publish a trigger event",
        usage: Some("auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>"),
        handler: Some(auto_emit_trigger),
        children: &[],
    },
];

/// Static app-claw command specs.
pub static COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "help",
        help: "List commands",
        usage: Some("help"),
        handler: Some(help),
        children: &[],
    },
    CommandSpec {
        name: "agent",
        help: "Agent and session operations",
        usage: Some("agent <command>"),
        handler: None,
        children: AGENT_COMMANDS,
    },
    CommandSpec {
        name: "cap",
        help: "Capability operations",
        usage: Some("cap <command>"),
        handler: None,
        children: CAP_COMMANDS,
    },
    CommandSpec {
        name: "auto",
        help: "Automation operations",
        usage: Some("auto <command>"),
        handler: None,
        children: AUTO_COMMANDS,
    },
];

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug, Default)]
    struct NullStream;

    impl ByteStream for NullStream {
        fn read(&mut self, _bytes: &mut [u8]) -> Result<usize> {
            Ok(0)
        }

        fn write(&mut self, bytes: &[u8]) -> Result<usize> {
            Ok(bytes.len())
        }

        fn flush(&mut self) -> Result<()> {
            Ok(())
        }
    }

    #[test]
    fn registers_command_tree() -> Result<()> {
        let mut console = DebugConsole::<NullStream>::new();
        register(&mut console)
    }

    #[test]
    fn parses_agent_session_modes() {
        assert_eq!(
            AgentSessionMode::parse("persist"),
            Some(AgentSessionMode::Persist)
        );
        assert_eq!(
            AgentSessionMode::parse("volatile"),
            Some(AgentSessionMode::Volatile)
        );
        assert_eq!(AgentSessionMode::parse("voliate"), None);
    }

    #[test]
    fn exposes_agent_group_instead_of_top_level_agent_aliases() {
        let mut has_agent = false;
        let mut has_top_level_ask = false;
        let mut has_top_level_session = false;

        for command in COMMANDS {
            match command.name {
                "agent" => has_agent = true,
                "ask" | "ask_once" => has_top_level_ask = true,
                "session" => has_top_level_session = true,
                _ => {}
            }
        }

        assert!(has_agent);
        assert!(!has_top_level_ask);
        assert!(!has_top_level_session);
    }

    #[test]
    fn wires_agent_session_commands_to_numeric_session_api() {
        let mut found = Vec::new();

        for command in COMMANDS {
            if command.name != "agent" {
                continue;
            }
            for child in command.children {
                if child.name != "session" {
                    continue;
                }
                assert!(child.handler.is_none());
                for session_child in child.children {
                    found.push(session_child.name);
                    assert!(session_child.handler.is_some());
                }
            }
        }

        assert_eq!(found, ["create", "list", "delete"]);
    }

    #[test]
    fn wires_agent_ask_to_numeric_session_api() {
        let mut found = false;

        for command in AGENT_COMMANDS {
            if command.name != "ask" {
                continue;
            }
            found = true;
            assert_eq!(
                command.usage,
                Some("agent ask <mode> <session_id> <prompt>")
            );
            assert!(command.handler.is_some());
        }

        assert!(found);
    }

    #[test]
    fn wires_capability_handlers_that_have_registry_apis() {
        let mut wired = Vec::new();
        let mut unwired = Vec::new();

        for command in CAP_COMMANDS {
            if command.handler.is_some() {
                wired.push(command.name);
            } else {
                unwired.push(command.name);
            }
        }

        assert_eq!(
            wired,
            ["list", "call", "groups", "enable", "disable", "unload"]
        );
        assert_eq!(unwired, ["load"]);
    }

    #[test]
    fn builds_json_object_string() {
        assert_eq!(
            json_object_string("rule_json", "{\"id\":\"a\"}\n"),
            "{\"rule_json\":\"{\\\"id\\\":\\\"a\\\"}\\n\"}"
        );
    }
}
