//! Built-ins owned by one agent: self-control and tool discovery.
//!
//! Orchestrator features such as multiagent are injected as ordinary
//! `ToolGroup`s during construction; they do not live in this module.
//!
//! Human approval is **not** a tool: it is raised by the permission layer (an
//! `Ask` decision in `base_agent`), not requested or resolved by the model.
//!
mod end_conversation;
mod tool_load;
mod tool_search;

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use claw_tool::{Tool, ToolDiscoveryHandle, ToolError, ToolGroup};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use end_conversation::EndConversationTool;
use tool_load::ToolLoadTool;
use tool_search::ToolSearchTool;

// -- Self-control seam ------------------------------------------------------

/// A signal an internal tool raises for the agent to act on next tick.
///
/// This is *internal*: it is not part of the public `AgentCommand` surface, so a
/// caller cannot forge an end-of-conversation.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
pub(crate) enum ControlSignal {
    /// The agent decided it is done; carries its closing message.
    EndConversation { final_message: String },
}

/// The shared queue internal tools push [`ControlSignal`]s onto.
///
/// The agent owns one; each internal tool handler holds a clone. A `Mutex`
/// (not a bare cell) because [`SyncToolHandler`](claw_tool::SyncToolHandler) is
/// `Send + Sync`; contention is nil in the single-driver-thread model.
pub(crate) type ControlSink = Arc<Mutex<VecDeque<ControlSignal>>>;

// -- Shared argument / rendering helpers -------------------------------------

/// Read one optional string argument out of a tool call.
///
/// # Errors
///
/// [`ToolError::InvalidArgumentsJson`] if the arguments are present but not valid JSON —
/// a malformed call is surfaced, not swallowed.
fn optional_string_argument(arguments_json: &str, key: &str) -> Result<Option<String>, ToolError> {
    let text = arguments_json.trim();
    let value = if text.is_empty() {
        Value::Object(serde_json::Map::new())
    } else {
        serde_json::from_str(text).map_err(|error| {
            ToolError::InvalidArgumentsJson(format!("invalid tool arguments JSON: {error}"))
        })?
    };
    if !value.is_object() {
        return Err(ToolError::InvalidArgumentsJson(
            "tool arguments must be a JSON object".into(),
        ));
    }
    match value.get(key) {
        Some(Value::String(text)) => Ok(Some(text.to_string())),
        Some(_) => Err(ToolError::InvalidArguments(format!(
            "'{key}' must be a string"
        ))),
        None => Ok(None),
    }
}

// -- Tool builders ----------------------------------------------------------

/// Build the agent's built-in tools over a control sink.
pub(crate) fn internal_tools(sink: ControlSink) -> ToolGroup {
    ToolGroup::new(
        "internal",
        true,
        [Tool::from_sync(EndConversationTool { sink })],
    )
}

/// Build the always-visible tool-discovery tools over a [`ToolSet`]'s discovery
/// bridge:
/// - `tool_search` — list the hidden tool groups that can be loaded;
/// - `tool_load` — reveal one group's tools for the next turn.
///
/// These keep the default tool surface small: the rest of an agent's tools stay
/// registered and searchable but hidden until `tool_load` reveals them.
pub(crate) fn discovery_tools(discovery: ToolDiscoveryHandle) -> ToolGroup {
    ToolGroup::new(
        "tool_discovery",
        true,
        [
            Tool::from_sync(ToolSearchTool {
                discovery: discovery.clone(),
            }),
            Tool::from_sync(ToolLoadTool { discovery }),
        ],
    )
}
