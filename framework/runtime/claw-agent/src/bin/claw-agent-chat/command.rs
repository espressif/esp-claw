use claw_agent::PermissionLevel;

const PERMISSIONS_COMMAND: &str = "/permissions";
const PERMISSION_LEVELS: &str = "<deny|ask|allow-all>";

#[derive(Debug, PartialEq, Eq)]
pub(super) enum CliInput<'a> {
    Message(&'a str),
    SetPermission(PermissionLevel),
}

#[derive(Debug, PartialEq, Eq, thiserror::Error)]
pub(super) enum CommandParseError {
    #[error("unknown command '/{0}'")]
    UnknownCommand(String),
    #[error("usage: /permissions {PERMISSION_LEVELS}")]
    MissingPermissionLevel,
    #[error("unknown permission level '{0}'; expected deny, ask, or allow-all")]
    UnknownPermissionLevel(String),
    #[error("unexpected argument '{0}'; usage: /permissions {PERMISSION_LEVELS}")]
    UnexpectedArgument(String),
}

pub(super) fn parse_input(input: &str) -> Result<CliInput<'_>, CommandParseError> {
    let input = input.trim();
    let Some(command_line) = input.strip_prefix('/') else {
        return Ok(CliInput::Message(input));
    };
    let mut parts = command_line.split_whitespace();
    let command = parts.next().unwrap_or_default();
    if command != "permissions" {
        return Err(CommandParseError::UnknownCommand(command.to_string()));
    }

    let level = parts
        .next()
        .ok_or(CommandParseError::MissingPermissionLevel)?;
    if let Some(argument) = parts.next() {
        return Err(CommandParseError::UnexpectedArgument(argument.to_string()));
    }

    let level = match level {
        "deny" => PermissionLevel::Deny,
        "ask" => PermissionLevel::Ask,
        "allow-all" => PermissionLevel::AllowAll,
        level => {
            return Err(CommandParseError::UnknownPermissionLevel(level.to_string()));
        }
    };
    Ok(CliInput::SetPermission(level))
}

pub(super) fn command_hint(line: &str, cursor: usize) -> Option<String> {
    if cursor != line.len() || !line.starts_with('/') {
        return None;
    }
    if line == "/" {
        return Some(format!("permissions {PERMISSION_LEVELS}"));
    }
    if let Some(suffix) = PERMISSIONS_COMMAND.strip_prefix(line) {
        return Some(format!("{suffix} {PERMISSION_LEVELS}"));
    }
    if line.strip_prefix(PERMISSIONS_COMMAND) == Some(" ") {
        return Some("deny | ask | allow-all".to_string());
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_text_is_a_message() {
        assert_eq!(
            parse_input("  explain this code  "),
            Ok(CliInput::Message("explain this code"))
        );
    }

    #[test]
    fn permissions_command_parses_every_level() {
        assert_eq!(
            parse_input("/permissions deny"),
            Ok(CliInput::SetPermission(PermissionLevel::Deny))
        );
        assert_eq!(
            parse_input("/permissions ask"),
            Ok(CliInput::SetPermission(PermissionLevel::Ask))
        );
        assert_eq!(
            parse_input("/permissions allow-all"),
            Ok(CliInput::SetPermission(PermissionLevel::AllowAll))
        );
    }

    #[test]
    fn invalid_commands_and_arguments_are_rejected() {
        assert_eq!(
            parse_input("/permissions"),
            Err(CommandParseError::MissingPermissionLevel)
        );
        assert_eq!(
            parse_input("/unknown"),
            Err(CommandParseError::UnknownCommand("unknown".to_string()))
        );
        assert_eq!(
            parse_input("/permissions maybe"),
            Err(CommandParseError::UnknownPermissionLevel(
                "maybe".to_string()
            ))
        );
        assert_eq!(
            parse_input("/permissions ask now"),
            Err(CommandParseError::UnexpectedArgument("now".to_string()))
        );
    }

    #[test]
    fn slash_input_has_contextual_hints() {
        assert_eq!(command_hint("", 0), None);
        assert_eq!(
            command_hint("/", 1).as_deref(),
            Some("permissions <deny|ask|allow-all>")
        );
        assert_eq!(
            command_hint("/perm", 5).as_deref(),
            Some("issions <deny|ask|allow-all>")
        );
        assert_eq!(
            command_hint("/permissions ", 13).as_deref(),
            Some("deny | ask | allow-all")
        );
        assert_eq!(command_hint("hello", 5), None);
        assert_eq!(command_hint("/permissions ", 4), None);
    }
}
