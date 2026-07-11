//! Model-callable tools for editable profile documents.

use core::str::FromStr;

use claw_interface::ClawFs;
use claw_memory::{ProfileDocument, ProfileStore};
use claw_permission::{Action, Resource, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolGroup, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolSpec,
};
use serde_json::Value;

/// Build the writable profile tools.
pub(crate) fn profile_tools<F: ClawFs + 'static>(store: ProfileStore<F>) -> ToolGroup {
    ToolGroup::new(
        "profile",
        true,
        [
            Tool::from_sync(ProfileReadTool {
                store: store.clone(),
            }),
            Tool::from_sync(ProfileReplaceTool {
                store: store.clone(),
            }),
            Tool::from_sync(ProfileClearTool { store }),
        ],
    )
}

struct ProfileReadTool<F: ClawFs + 'static> {
    store: ProfileStore<F>,
}

impl<F: ClawFs + 'static> ToolSpec for ProfileReadTool<F> {
    tool_metadata!("profile_read");

    fn concurrent(&self) -> bool {
        true
    }

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        profile_action(call, "profile_read", RiskClass::Safe, &self.store)
    }
}

impl<F: ClawFs + 'static> SyncToolHandler for ProfileReadTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let document = document_from_args(&args)?;
        match self.store.read(document) {
            Ok(Some(content)) => Ok(ToolOutput {
                output: if content.trim().is_empty() {
                    format!("Profile document {document} is empty.")
                } else {
                    format!("Profile document {document}:\n{content}")
                },
                ok: true,
            }),
            Ok(None) => Ok(ToolOutput {
                output: format!("Profile document {document} does not exist."),
                ok: true,
            }),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not read profile document {document}: {error}."),
                ok: false,
            }),
        }
    }
}

struct ProfileReplaceTool<F: ClawFs + 'static> {
    store: ProfileStore<F>,
}

impl<F: ClawFs + 'static> ToolSpec for ProfileReplaceTool<F> {
    tool_metadata!("profile_replace");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        profile_action(call, "profile_replace", RiskClass::High, &self.store)
    }
}

impl<F: ClawFs + 'static> SyncToolHandler for ProfileReplaceTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let document = document_from_args(&args)?;
        let content = args.get("content").and_then(Value::as_str).ok_or_else(|| {
            ToolInvokeError::new(ToolError::InvokeRejected(
                "missing required string field 'content'".into(),
            ))
        })?;
        match self.store.replace(document, content) {
            Ok(()) => Ok(ToolOutput {
                output: format!("Replaced profile document {document}."),
                ok: true,
            }),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not replace profile document {document}: {error}."),
                ok: false,
            }),
        }
    }
}

struct ProfileClearTool<F: ClawFs + 'static> {
    store: ProfileStore<F>,
}

impl<F: ClawFs + 'static> ToolSpec for ProfileClearTool<F> {
    tool_metadata!("profile_clear");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        profile_action(call, "profile_clear", RiskClass::High, &self.store)
    }
}

impl<F: ClawFs + 'static> SyncToolHandler for ProfileClearTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let document = document_from_args(&args)?;
        match self.store.clear(document) {
            Ok(()) => Ok(ToolOutput {
                output: format!("Cleared profile document {document}."),
                ok: true,
            }),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not clear profile document {document}: {error}."),
                ok: false,
            }),
        }
    }
}

fn profile_action<F: ClawFs + 'static>(
    call: &ToolInvocation<'_>,
    verb: &str,
    risk: RiskClass,
    store: &ProfileStore<F>,
) -> Action {
    let action = Action::new(verb, risk);
    let Ok(args) = parse_object(call) else {
        return action;
    };
    let Ok(document) = document_from_args(&args) else {
        return action;
    };
    action.with_resource(Resource::Path(store.path(document)))
}

fn parse_object(call: &ToolInvocation<'_>) -> Result<Value, ToolInvokeError> {
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

fn document_from_args(args: &Value) -> Result<ProfileDocument, ToolInvokeError> {
    let document = args
        .get("document")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|text| !text.is_empty())
        .ok_or_else(|| {
            ToolInvokeError::new(ToolError::InvokeRejected(
                "missing required string field 'document'".into(),
            ))
        })?;
    ProfileDocument::from_str(document).map_err(|error| {
        ToolInvokeError::new(ToolError::InvokeRejected(format!(
            "{error}; expected one of: soul, assistant_identity, user_profile"
        )))
    })
}
