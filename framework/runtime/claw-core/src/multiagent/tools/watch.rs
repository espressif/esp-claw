use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use super::super::tool_port::SubagentControl;
use super::args::{action_with_agent_resource, required_agent_id};

pub(super) fn tool(control: Arc<SubagentControl>) -> Tool {
    Tool::from_sync(WatchSubagentTool { control })
}

struct WatchSubagentTool {
    control: Arc<SubagentControl>,
}

impl ToolSpec for WatchSubagentTool {
    tool_metadata!("subagent_watch");

    fn concurrent(&self) -> bool {
        true
    }

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        action_with_agent_resource("subagent_watch", RiskClass::Safe, call)
    }
}

impl SyncToolHandler for WatchSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let target = required_agent_id(call, "subagent_watch")?;
        match self.control.get(target) {
            Some(snapshot) => Ok(ToolOutput {
                output: serde_json::to_string(&snapshot).map_err(|error| {
                    ToolError::InvokeRejected(format!(
                        "failed to serialize subagent snapshot: {error}"
                    ))
                })?,
                ok: true,
            }),
            None => Ok(ToolOutput {
                output: format!(
                    "No subagent {target} in your subtree (unknown id, or not one of yours)."
                ),
                ok: false,
            }),
        }
    }
}
