//! Argument parsing and JSON Schema validation against each tool's baked
//! `function.parameters` schema.

use jsonschema::Validator;
use serde_json::Value;

use crate::handler::{tool_invoke_err, ToolError, ToolInvokeError};

use crate::set::ToolSetError;

/// Extract `function.parameters` from an OpenAI-style tool schema text.
///
/// When `parameters` is absent, returns a permissive empty-object schema so
/// tools with no declared parameters still validate consistently.
pub(crate) fn parameters_from_tool_schema(schema_text: &str) -> Result<Value, String> {
    let root: Value = serde_json::from_str(schema_text)
        .map_err(|error| format!("invalid tool schema JSON: {error}"))?;
    if !root.is_object() {
        return Err("tool schema must be a JSON object".into());
    }
    let parameters = root
        .get("function")
        .and_then(|function| function.get("parameters"))
        .cloned()
        .unwrap_or_else(|| serde_json::json!({"type": "object", "properties": {}}));
    Ok(parameters)
}

/// Compile a validator for one tool's parameter schema at [`ToolSet`] assembly time.
pub(crate) fn compile_argument_validator(
    tool_name: &str,
    schema_text: &str,
) -> Result<Validator, ToolSetError> {
    let parameters = parameters_from_tool_schema(schema_text).map_err(|details| {
        ToolSetError::InvalidParameterSchema {
            tool: tool_name.to_string(),
            details,
        }
    })?;
    Validator::new(&parameters).map_err(|error| ToolSetError::InvalidParameterSchema {
        tool: tool_name.to_string(),
        details: error.to_string(),
    })
}

/// Parse model tool-call arguments into a JSON object (`""` ⇒ `{}`).
pub(crate) fn parse_arguments_json(arguments_json: &str) -> Result<Value, ToolInvokeError> {
    let trimmed = arguments_json.trim();
    let text = if trimmed.is_empty() { "{}" } else { trimmed };
    let value: Value = serde_json::from_str(text)
        .map_err(|error| tool_invoke_err(ToolError::InvalidArgumentsJson(error.to_string())))?;
    if !value.is_object() {
        return Err(tool_invoke_err(ToolError::InvalidArgumentsJson(
            "tool arguments must be a JSON object".into(),
        )));
    }
    Ok(value)
}

/// Validate a parsed arguments object against a compiled parameter validator.
pub(crate) fn validate_arguments(
    validator: &Validator,
    arguments: &Value,
) -> Result<(), ToolInvokeError> {
    validator
        .validate(arguments)
        .map_err(|error| tool_invoke_err(ToolError::InvalidArguments(error.to_string())))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    const SPAWN_SCHEMA: &str = r#"{
        "type": "function",
        "function": {
            "name": "spawn_subagent",
            "parameters": {
                "type": "object",
                "properties": {
                    "kind": { "type": "string" },
                    "name": { "type": "string" },
                    "goal": { "type": "string" }
                },
                "required": ["kind", "name", "goal"]
            }
        }
    }"#;

    #[test]
    fn missing_parameters_defaults_to_empty_object_schema() {
        let schema = r#"{"type":"function","function":{"name":"noop"}}"#;
        let parameters = parameters_from_tool_schema(schema).unwrap();
        assert_eq!(parameters["type"], "object");
        let validator = Validator::new(&parameters).unwrap();
        assert!(validator.validate(&serde_json::json!({})).is_ok());
    }

    #[test]
    fn required_fields_are_enforced() {
        let parameters = parameters_from_tool_schema(SPAWN_SCHEMA).unwrap();
        let validator = Validator::new(&parameters).unwrap();
        let args = serde_json::json!({"kind": "worker", "goal": "x"});
        let err = validator.validate(&args).unwrap_err();
        assert!(err.to_string().contains("name"));
    }

    #[test]
    fn empty_arguments_json_parses_as_object() {
        let value = parse_arguments_json("").unwrap();
        assert_eq!(value, serde_json::json!({}));
    }
}
