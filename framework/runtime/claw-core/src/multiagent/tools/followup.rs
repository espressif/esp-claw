use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use crate::protocol::Message;

use super::super::tool_port::SubagentControl;
use super::args::{action_with_agent_resource, non_blank_argument, required_agent_id};

pub(super) fn tool(control: Arc<SubagentControl>) -> Tool {
    Tool::from_sync(FollowupSubagentTool { control })
}

struct FollowupSubagentTool {
    control: Arc<SubagentControl>,
}

impl ToolSpec for FollowupSubagentTool {
    tool_metadata!("subagent_followup");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        action_with_agent_resource("subagent_followup", RiskClass::Moderate, call)
    }
}

impl SyncToolHandler for FollowupSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let target = required_agent_id(call, "subagent_followup")?;
        let message = Message::text(non_blank_argument(call.arguments_json(), "message")?);
        if self.control.get(target).is_none() {
            return Ok(ToolOutput {
                output: format!("Cannot follow up {target}: it is not a subagent in your subtree."),
                ok: false,
            });
        }
        self.control.followup(target, message);
        Ok(ToolOutput {
            output: format!("Subagent {target} retasked with new input."),
            ok: true,
        })
    }
}
