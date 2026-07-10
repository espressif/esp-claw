//! `subagent_followup(agent, message)` — cancel the target's current task and
//! start a new one with `message`.

use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::AgentContext;

use super::{agent_resource, optional_string_argument};

/// Read one required non-blank string argument.
fn required_message(arguments_json: &str, key: &str) -> Result<String, ToolError> {
    let Some(raw) = optional_string_argument(arguments_json, key)? else {
        return Err(ToolError::InvalidArguments(format!(
            "subagent_followup '{key}' is required"
        )));
    };
    if raw.is_empty() {
        return Err(ToolError::InvalidArguments(format!(
            "subagent_followup '{key}' is required"
        )));
    }
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Err(ToolError::InvokeRejected(format!(
            "subagent_followup '{key}' must not be blank"
        )));
    }
    Ok(trimmed.to_string())
}

/// Retasks one descendant: cancel its in-flight work, then deliver a new goal.
pub(crate) struct FollowupSubagentTool {
    pub(super) context: Arc<AgentContext>,
}

impl ToolSpec for FollowupSubagentTool {
    tool_metadata!("subagent_followup");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        let action = Action::new("subagent_followup", RiskClass::Moderate);
        match agent_resource(call) {
            Some(resource) => action.with_resource(resource),
            None => action,
        }
    }
}

impl SyncToolHandler for FollowupSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let Some(agent) = optional_string_argument(call.arguments_json(), "agent")? else {
            return Err(ToolError::InvalidArguments(
                "subagent_followup 'agent' is required".into(),
            )
            .into());
        };
        let agent = agent.trim();
        if agent.is_empty() {
            return Err(ToolError::InvalidArguments(
                "subagent_followup 'agent' is required".into(),
            )
            .into());
        }
        let target = AgentId::from_wire(agent).map_err(|error| {
            ToolError::InvokeRejected(format!("invalid agent id '{agent}': {error}"))
        })?;
        let message = required_message(call.arguments_json(), "message")?;
        if self.context.get_subagent(target).is_none() {
            return Ok(ToolOutput {
                output: format!("Cannot follow up {target}: it is not a subagent in your subtree."),
                ok: false,
            });
        }
        self.context.followup_subagent(target, message.clone());
        Ok(ToolOutput {
            output: format!("Subagent {target} retasked with new input."),
            ok: true,
        })
    }
}
