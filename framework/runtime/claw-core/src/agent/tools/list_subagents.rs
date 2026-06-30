//! `list_subagents()` — enumerate this agent's subtree.

use std::sync::Arc;

use claw_tool::{tool_metadata, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};
use serde_json::Value;

use crate::agent::graph::AgentContext;

use super::snapshot_json;

/// Reads the agent's subtree snapshot through the graph context.
pub(crate) struct ListSubagentsTool {
    context: Arc<AgentContext>,
}

impl ListSubagentsTool {
    /// Build the tool over the agent's `context`.
    pub(crate) fn new(context: Arc<AgentContext>) -> Self {
        Self { context }
    }
}

impl ToolHandler for ListSubagentsTool {
    tool_metadata!("list_subagents");

    // A pure read of the graph snapshot — safe to run alongside other calls.
    fn concurrent(&self) -> bool {
        true
    }

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let subagents: Vec<Value> = self
            .context
            .list_subagents()
            .iter()
            .map(snapshot_json)
            .collect();
        Ok(ToolOutput {
            output: serde_json::json!({ "subagents": subagents }).to_string(),
            ok: true,
        })
    }
}
