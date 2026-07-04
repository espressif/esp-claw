use serde_json::Value;

use super::tool::{ToolError, ToolInvokeError, ToolResult};

pub(super) fn normalize_arguments_json(arguments_json: &str) -> ToolResult<&str> {
    let text = normalized_arguments_json(arguments_json);
    parse_arguments_json(text)?;
    Ok(text)
}

pub(super) fn parse_arguments_json(arguments_json: &str) -> ToolResult<Value> {
    let text = normalized_arguments_json(arguments_json);
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

fn normalized_arguments_json(arguments_json: &str) -> &str {
    let text = arguments_json.trim();
    if text.is_empty() {
        "{}"
    } else {
        text
    }
}
