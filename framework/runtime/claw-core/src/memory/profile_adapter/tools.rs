//! Model-callable tools for editable profile documents.

use core::str::FromStr;

use claw_interface::ClawFs;
use claw_memory::{ProfileDocument, ProfileStore};
use claw_permission::{Action, Resource, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};
use serde_json::Value;

/// Build the writable profile tools.
pub(crate) fn profile_tools<F: ClawFs + 'static>(store: ProfileStore<F>) -> Vec<Tool> {
    vec![
        Tool::from_sync(ProfileReadTool {
            store: store.clone(),
        }),
        Tool::from_sync(ProfileReplaceTool {
            store: store.clone(),
        }),
        Tool::from_sync(ProfileClearTool { store }),
    ]
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
                output: render_document(document, &content),
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
        let content = required_raw_string(&args, "content")?;
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
    if call.arguments_json().trim().is_empty() {
        return Ok(Value::Object(serde_json::Map::new()));
    }
    serde_json::from_str(call.arguments_json())
        .map_err(|error| ToolInvokeError::new(ToolError::InvalidArgumentsJson(error.to_string())))
}

fn document_from_args(args: &Value) -> Result<ProfileDocument, ToolInvokeError> {
    let document = required_trimmed_string(args, "document")?;
    ProfileDocument::from_str(&document).map_err(|error| {
        ToolInvokeError::new(ToolError::InvokeRejected(format!(
            "{error}; expected one of: soul, assistant_identity, user_profile"
        )))
    })
}

fn required_trimmed_string(args: &Value, key: &str) -> Result<String, ToolInvokeError> {
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

fn required_raw_string<'a>(args: &'a Value, key: &str) -> Result<&'a str, ToolInvokeError> {
    args.get(key).and_then(Value::as_str).ok_or_else(|| {
        ToolInvokeError::new(ToolError::InvokeRejected(format!(
            "missing required string field '{key}'"
        )))
    })
}

fn render_document(document: ProfileDocument, content: &str) -> String {
    if content.trim().is_empty() {
        return format!("Profile document {document} is empty.");
    }
    format!("Profile document {document}:\n{content}")
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use claw_interface::MemFs;
    use claw_tool::RawToolInvocation;

    use super::*;

    fn store() -> ProfileStore<MemFs> {
        MemFs::new();
        ProfileStore::new("/memory")
    }

    fn call<'a>(name: &'a str, arguments_json: &'a str) -> ToolInvocation<'a> {
        ToolInvocation::try_from(RawToolInvocation {
            id: None,
            name,
            arguments_json,
        })
        .unwrap()
    }

    #[test]
    fn read_returns_document_content() {
        let store = store();
        store.replace(ProfileDocument::Soul, "SOUL").unwrap();
        let tool = ProfileReadTool { store };
        let output = tool
            .invoke(&call("profile_read", r#"{"document":"soul"}"#))
            .unwrap();
        assert!(output.ok);
        assert!(output.output.contains("SOUL"));
    }

    #[test]
    fn replace_updates_the_store() {
        let store = store();
        let tool = ProfileReplaceTool {
            store: store.clone(),
        };
        let output = tool
            .invoke(&call(
                "profile_replace",
                r#"{"document":"user_profile","content":"USER"}"#,
            ))
            .unwrap();
        assert!(output.ok);
        assert_eq!(
            store.read(ProfileDocument::UserProfile).unwrap(),
            Some("USER".to_string())
        );
    }

    #[test]
    fn clear_empties_the_document() {
        let store = store();
        store
            .replace(ProfileDocument::AssistantIdentity, "ID")
            .unwrap();
        let tool = ProfileClearTool {
            store: store.clone(),
        };
        let output = tool
            .invoke(&call(
                "profile_clear",
                r#"{"document":"assistant_identity"}"#,
            ))
            .unwrap();
        assert!(output.ok);
        assert_eq!(
            store.read(ProfileDocument::AssistantIdentity).unwrap(),
            Some(String::new())
        );
    }
}
