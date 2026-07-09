//! `list_subagents()` — enumerate this agent's subtree.

use std::sync::Arc;

use claw_tool::{
    tool_metadata, SyncToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use crate::agent::graph::AgentContext;

/// Reads the agent's subtree snapshot through the graph context.
pub(crate) struct ListSubagentsTool {
    pub(super) context: Arc<AgentContext>,
}

impl ToolSpec for ListSubagentsTool {
    tool_metadata!("list_subagents");

    // A pure read of the graph snapshot — safe to run alongside other calls.
    fn concurrent(&self) -> bool {
        true
    }
}

impl SyncToolHandler for ListSubagentsTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let subagents = self.context.list_subagents();
        Ok(ToolOutput {
            output: serde_json::json!({ "subagents": subagents }).to_string(),
            ok: true,
        })
    }
}
