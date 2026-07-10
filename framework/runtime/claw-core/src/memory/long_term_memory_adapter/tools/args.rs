use claw_tool::{ToolError, ToolInvocation, ToolInvokeError};
use serde_json::Value;

const DEFAULT_RECALL_LIMIT: usize = 20;

pub(super) fn parse_object(call: &ToolInvocation<'_>) -> Result<Value, ToolInvokeError> {
    let text = call.arguments_json().trim();
    let value = if text.is_empty() {
        Value::Object(serde_json::Map::new())
    } else {
        serde_json::from_str(text).map_err(|error| {
            ToolInvokeError::new(ToolError::InvalidArgumentsJson(error.to_string()))
        })?
    };
    if !value.is_object() {
        return Err(ToolInvokeError::new(ToolError::InvalidArgumentsJson(
            "tool arguments must be a JSON object".into(),
        )));
    }
    Ok(value)
}

pub(super) fn required_string(args: &Value, key: &str) -> Result<String, ToolInvokeError> {
    let value = args
        .get(key)
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|text| !text.is_empty())
        .ok_or_else(|| {
            ToolInvokeError::new(ToolError::InvokeRejected(format!(
                "missing required string field '{key}'"
            )))
        })?;
    Ok(value.to_string())
}

pub(super) fn optional_string(args: &Value, key: &str) -> Result<Option<String>, ToolInvokeError> {
    match args.get(key) {
        Some(Value::String(value)) => {
            let value = value.trim();
            if value.is_empty() {
                Ok(None)
            } else {
                Ok(Some(value.to_string()))
            }
        }
        Some(_) => Err(ToolInvokeError::new(ToolError::InvalidArguments(format!(
            "'{key}' must be a string"
        )))),
        None => Ok(None),
    }
}

pub(super) fn string_array(args: &Value, key: &str) -> Result<Vec<String>, ToolInvokeError> {
    match optional_string_array(args, key)? {
        Some(values) => Ok(values),
        None => Ok(Vec::new()),
    }
}

pub(super) fn optional_string_array(
    args: &Value,
    key: &str,
) -> Result<Option<Vec<String>>, ToolInvokeError> {
    let Some(value) = args.get(key) else {
        return Ok(None);
    };
    let Some(items) = value.as_array() else {
        return Err(ToolInvokeError::new(ToolError::InvalidArguments(format!(
            "'{key}' must be an array of strings"
        ))));
    };
    let mut strings = Vec::with_capacity(items.len());
    for item in items {
        let Some(text) = item.as_str() else {
            return Err(ToolInvokeError::new(ToolError::InvalidArguments(format!(
                "'{key}' must be an array of strings"
            ))));
        };
        let text = text.trim();
        if !text.is_empty() {
            strings.push(text.to_string());
        }
    }
    Ok(Some(strings))
}

pub(super) fn optional_limit(args: &Value) -> Result<usize, ToolInvokeError> {
    let Some(value) = args.get("limit") else {
        return Ok(DEFAULT_RECALL_LIMIT);
    };
    let Some(limit) = value.as_u64() else {
        return Err(ToolInvokeError::new(ToolError::InvalidArguments(
            "'limit' must be an unsigned integer".into(),
        )));
    };
    if limit == 0 {
        return Ok(DEFAULT_RECALL_LIMIT);
    }
    usize::try_from(limit).map_err(|_| {
        ToolInvokeError::new(ToolError::InvalidArguments(
            "'limit' is too large for this platform".into(),
        ))
    })
}
