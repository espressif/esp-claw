//! `subagent.watch(agent)` — snapshot one descendant by id.

use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::AgentContext;

use super::{agent_resource, optional_string_argument};

/// Reads one descendant's snapshot through the graph context.
pub(crate) struct WatchSubagentTool {
    pub(super) context: Arc<AgentContext>,
}

impl ToolSpec for WatchSubagentTool {
    tool_metadata!("subagent.watch");

    // A pure read of one snapshot — safe to run alongside other calls.
    fn concurrent(&self) -> bool {
        true
    }

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        let action = Action::new("subagent.watch", RiskClass::Safe);
        match agent_resource(call) {
            Some(resource) => action.with_resource(resource),
            None => action,
        }
    }
}

impl SyncToolHandler for WatchSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let Some(agent) = optional_string_argument(call.arguments_json(), "agent")? else {
            return Err(
                ToolError::InvalidArguments("subagent.watch 'agent' is required".into()).into(),
            );
        };
        let agent = agent.trim();
        if agent.is_empty() {
            return Err(
                ToolError::InvalidArguments("subagent.watch 'agent' is required".into()).into(),
            );
        }
        let target = AgentId::from_wire(agent).map_err(|error| {
            ToolError::InvokeRejected(format!("invalid agent id '{agent}': {error}"))
        })?;
        match self.context.get_subagent(target) {
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
