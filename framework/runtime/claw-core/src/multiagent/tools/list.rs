use std::sync::Arc;

use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use super::super::tool_port::SubagentControl;

pub(super) fn tool(control: Arc<SubagentControl>) -> Tool {
    Tool::from_sync(ListSubagentsTool { control })
}

struct ListSubagentsTool {
    control: Arc<SubagentControl>,
}

impl ToolSpec for ListSubagentsTool {
    tool_metadata!("subagent_list");

    fn concurrent(&self) -> bool {
        true
    }
}

impl SyncToolHandler for ListSubagentsTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: serde_json::json!({ "subagents": self.control.list() }).to_string(),
            ok: true,
        })
    }
}
