use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use super::super::tool_port::SubagentControl;
use super::args::{action_with_agent_resource, required_agent_id};

pub(super) fn tool(control: Arc<SubagentControl>) -> Tool {
    Tool::from_sync(DeleteSubagentTool { control })
}

struct DeleteSubagentTool {
    control: Arc<SubagentControl>,
}

impl ToolSpec for DeleteSubagentTool {
    tool_metadata!("subagent_delete");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        action_with_agent_resource("subagent_delete", RiskClass::High, call)
    }
}

impl SyncToolHandler for DeleteSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let target = required_agent_id(call, "subagent_delete")?;
        if self.control.get(target).is_none() {
            return Ok(ToolOutput {
                output: format!("Cannot delete {target}: it is not a subagent in your subtree."),
                ok: false,
            });
        }
        self.control.delete(target);
        Ok(ToolOutput {
            output: format!("Subagent {target} and its subtree scheduled for deletion."),
            ok: true,
        })
    }
}
