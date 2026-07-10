//! `subagent_delete(agent)` — remove one descendant (and its subtree) by id.

use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::AgentContext;

use super::{agent_resource, optional_string_argument};

/// Requests removal of one descendant (and its subtree) through the graph context.
pub(crate) struct DeleteSubagentTool {
    pub(super) context: Arc<AgentContext>,
}

impl ToolSpec for DeleteSubagentTool {
    tool_metadata!("subagent_delete");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        // Removing an agent and its subtree is irreversible — high risk.
        let action = Action::new("subagent_delete", RiskClass::High);
        match agent_resource(call) {
            Some(resource) => action.with_resource(resource),
            None => action,
        }
    }
}

impl SyncToolHandler for DeleteSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let Some(agent) = optional_string_argument(call.arguments_json(), "agent")? else {
            return Err(
                ToolError::InvalidArguments("subagent_delete 'agent' is required".into()).into(),
            );
        };
        let agent = agent.trim();
        if agent.is_empty() {
            return Err(
                ToolError::InvalidArguments("subagent_delete 'agent' is required".into()).into(),
            );
        }
        let target = AgentId::from_wire(agent).map_err(|error| {
            ToolError::InvokeRejected(format!("invalid agent id '{agent}': {error}"))
        })?;
        // Authorize against the same subtree view watch uses, so the refusal is
        // immediate (the instance re-checks before actually removing).
        if self.context.get_subagent(target).is_none() {
            return Ok(ToolOutput {
                output: format!("Cannot delete {target}: it is not a subagent in your subtree."),
                ok: false,
            });
        }
        self.context.delete_subagent(target);
        Ok(ToolOutput {
            output: format!("Subagent {target} and its subtree scheduled for deletion."),
            ok: true,
        })
    }
}
