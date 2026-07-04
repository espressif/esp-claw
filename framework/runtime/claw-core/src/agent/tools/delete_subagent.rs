//! `delete_subagent(agent)` — remove one descendant (and its subtree) by id.

use std::sync::Arc;

use claw_capability::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};
use claw_permission::{Action, RiskClass};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::AgentContext;

use super::{agent_resource, string_argument};

/// Requests removal of one descendant (and its subtree) through the graph context.
pub(crate) struct DeleteSubagentTool {
    context: Arc<AgentContext>,
}

impl DeleteSubagentTool {
    /// Build the tool over the agent's `context`.
    pub(crate) fn new(context: Arc<AgentContext>) -> Self {
        Self { context }
    }
}

impl ToolSpec for DeleteSubagentTool {
    tool_metadata!("delete_subagent");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        // Removing an agent and its subtree is irreversible — high risk.
        let action = Action::new("delete_subagent", RiskClass::High);
        match agent_resource(call) {
            Some(resource) => action.with_resource(resource),
            None => action,
        }
    }
}

impl SyncToolHandler for DeleteSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let agent = string_argument(call.arguments_json(), "agent")?;
        let target = AgentId::from_wire(agent.trim()).map_err(|error| {
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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::agent::graph::test_support::{context_for, host_with_tree, snap};
    use crate::agent::graph::{GraphEffect, GraphHost};
    use claw_capability::RawToolInvocation;

    fn call<'a>(id: &'a str, arguments_json: &'a str) -> ToolInvocation<'a> {
        ToolInvocation::try_from(RawToolInvocation {
            id: Some(id),
            name: "delete_subagent",
            arguments_json,
        })
        .unwrap()
    }

    #[test]
    fn delete_subagent_emits_only_for_a_descendant() {
        let host = host_with_tree(vec![
            snap(1, None, 0),
            snap(2, Some(1), 1),
            snap(3, Some(2), 2),
        ]);
        let context = context_for(Arc::clone(&host) as Arc<dyn GraphHost>, AgentId(2));
        let delete = DeleteSubagentTool::new(context);

        let ok = delete
            .invoke(&call("d1", r#"{"agent":"agent-3"}"#))
            .unwrap();
        assert!(ok.ok);

        let refused = delete
            .invoke(&call("d2", r#"{"agent":"agent-1"}"#))
            .unwrap();
        assert!(!refused.ok);

        // Only the descendant delete was emitted.
        let effects = host.effects.lock().unwrap();
        assert_eq!(
            effects.as_slice(),
            &[(AgentId(2), GraphEffect::Delete { target: AgentId(3) })]
        );
    }
}
