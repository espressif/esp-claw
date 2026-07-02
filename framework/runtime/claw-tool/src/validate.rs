//! Argument parsing for model tool-call arguments.

use serde_json::Value;

use crate::handler::{ToolError, ToolInvokeError};

/// Parse model tool-call arguments into a JSON object (`""` ⇒ `{}`).
pub(crate) fn parse_arguments_json(arguments_json: &str) -> Result<Value, ToolInvokeError> {
    let trimmed = arguments_json.trim();
    let text = if trimmed.is_empty() { "{}" } else { trimmed };
    let value: Value = serde_json::from_str(text).map_err(|error| {
        ToolInvokeError::new(ToolError::InvalidArgumentsJson(error.to_string()))
    })?;
    if !value.is_object() {
        return Err(ToolInvokeError::new(ToolError::InvalidArgumentsJson(
            "tool arguments must be a JSON object".into(),
        )));
    }
    Ok(value)
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn empty_arguments_json_parses_as_object() {
        let value = parse_arguments_json("").unwrap();
        assert_eq!(value, serde_json::json!({}));
    }

    #[test]
    fn non_object_arguments_are_rejected() {
        let error = parse_arguments_json("[]").unwrap_err();
        assert!(matches!(error.error, ToolError::InvalidArgumentsJson(_)));
    }
}
