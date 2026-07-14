use claw_permission::{Action, Resource, RiskClass};
use claw_tool::{ToolError, ToolInvocation, ToolInvokeError};
use serde_json::Value;

use crate::protocol::AgentId;

pub(super) fn non_blank_argument(arguments_json: &str, key: &str) -> Result<String, ToolError> {
    let Some(raw) = optional_string_argument(arguments_json, key)? else {
        return Err(ToolError::InvalidArguments(format!("'{key}' is required")));
    };
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Err(ToolError::InvokeRejected(format!(
            "'{key}' must not be blank"
        )));
    }
    Ok(trimmed.to_string())
}

pub(super) fn required_bool_argument(arguments_json: &str, key: &str) -> Result<bool, ToolError> {
    let value = arguments_object(arguments_json)?;
    match value.get(key) {
        Some(Value::Bool(value)) => Ok(*value),
        Some(_) => Err(ToolError::InvalidArguments(format!(
            "'{key}' must be a boolean"
        ))),
        None => Err(ToolError::InvalidArguments(format!("'{key}' is required"))),
    }
}

pub(super) fn required_agent_id(
    call: &ToolInvocation<'_>,
    tool: &str,
) -> Result<AgentId, ToolInvokeError> {
    let raw = optional_string_argument(call.arguments_json(), "agent")?
        .ok_or_else(|| ToolError::InvalidArguments(format!("{tool} 'agent' is required")))?;
    let agent = raw.trim();
    if agent.is_empty() {
        return Err(ToolError::InvalidArguments(format!("{tool} 'agent' is required")).into());
    }
    AgentId::from_wire(agent)
        .map_err(|error| ToolError::InvokeRejected(format!("invalid agent id '{agent}': {error}")))
        .map_err(Into::into)
}

pub(super) fn action_with_agent_resource(
    name: &'static str,
    risk: RiskClass,
    call: &ToolInvocation<'_>,
) -> Action {
    let action = Action::new(name, risk);
    match agent_resource(call) {
        Some(resource) => action.with_resource(resource),
        None => action,
    }
}

fn optional_string_argument(arguments_json: &str, key: &str) -> Result<Option<String>, ToolError> {
    let value = arguments_object(arguments_json)?;
    match value.get(key) {
        Some(Value::String(text)) => Ok(Some(text.to_string())),
        Some(_) => Err(ToolError::InvalidArguments(format!(
            "'{key}' must be a string"
        ))),
        None => Ok(None),
    }
}

fn arguments_object(arguments_json: &str) -> Result<Value, ToolError> {
    let text = arguments_json.trim();
    let value = if text.is_empty() {
        Value::Object(serde_json::Map::new())
    } else {
        serde_json::from_str(text).map_err(|error| {
            ToolError::InvalidArgumentsJson(format!("invalid tool arguments JSON: {error}"))
        })?
    };
    if value.is_object() {
        Ok(value)
    } else {
        Err(ToolError::InvalidArgumentsJson(
            "tool arguments must be a JSON object".into(),
        ))
    }
}

fn agent_resource(call: &ToolInvocation<'_>) -> Option<Resource> {
    let raw = optional_string_argument(call.arguments_json(), "agent")
        .ok()
        .flatten()?;
    let trimmed = raw.trim();
    (!trimmed.is_empty()).then(|| Resource::Agent(trimmed.to_string()))
}
