//! app-claw debug console commands.

use debug_console::{
    ByteStream, CommandArgs, CommandContext, CommandSpec, ConsoleRuntime, DebugConsole, Result,
};

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

static AGENT_COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "ask",
        help: "Submit a prompt to an explicit session",
        usage: Some("agent ask <mode> <session_id> <prompt>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "session",
        help: "Show one agent session",
        usage: Some("agent session <session_id>"),
        handler: None,
        children: &[],
    },
];

static CAP_COMMANDS: &[CommandSpec] = &[
    CommandSpec {
        name: "list",
        help: "List capabilities",
        usage: Some("cap list"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "call",
        help: "Call one capability",
        usage: Some("cap call <name> <json>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "groups",
        help: "List capability groups",
        usage: Some("cap groups"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "enable",
        help: "Enable a capability group",
        usage: Some("cap enable <group_id>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "disable",
        help: "Disable a capability group",
        usage: Some("cap disable <group_id>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "unload",
        help: "Unload a capability group",
        usage: Some("cap unload <group_id>"),
        handler: None,
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
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "rules",
        help: "List automation rules",
        usage: Some("auto rules"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "rule",
        help: "Show one automation rule",
        usage: Some("auto rule <id>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "last",
        help: "Show last automation result",
        usage: Some("auto last"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "add_rule",
        help: "Add one automation rule",
        usage: Some("auto add_rule <json>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "update_rule",
        help: "Update one automation rule",
        usage: Some("auto update_rule <json>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "delete_rule",
        help: "Delete one automation rule",
        usage: Some("auto delete_rule <id>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "emit_message",
        help: "Publish a message event",
        usage: Some("auto emit_message <source_cap> <channel> <chat_id> <text>"),
        handler: None,
        children: &[],
    },
    CommandSpec {
        name: "emit_trigger",
        help: "Publish a trigger event",
        usage: Some("auto emit_trigger <source_cap> <event_type> <event_key> <payload_json>"),
        handler: None,
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
    fn leaves_unwired_handlers_empty() {
        let mut found = false;

        for command in COMMANDS {
            if command.name != "agent" {
                continue;
            }
            for child in command.children {
                if child.name != "ask" {
                    continue;
                }
                found = true;
                assert_eq!(child.usage, Some("agent ask <mode> <session_id> <prompt>"));
                assert!(child.handler.is_none());
            }
        }

        assert!(found);
    }
}
