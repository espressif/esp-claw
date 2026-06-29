//! `watch_subagent(agent)` — snapshot one descendant by id.

use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_invoke_err, tool_metadata, ToolError, ToolHandler, ToolInvocation, ToolInvokeError,
    ToolOutput,
};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::AgentContext;

use super::{agent_resource, snapshot_json, string_argument};

/// Reads one descendant's snapshot through the graph context.
pub(crate) struct WatchSubagentTool {
    context: Arc<AgentContext>,
}

impl WatchSubagentTool {
    /// Build the tool over the agent's `context`.
    pub(crate) fn new(context: Arc<AgentContext>) -> Self {
        Self { context }
    }
}

impl ToolHandler for WatchSubagentTool {
    tool_metadata!("watch_subagent");

    // A pure read of one snapshot — safe to run alongside other calls.
    fn concurrent(&self) -> bool {
        true
    }

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        let action = Action::new("watch_subagent", RiskClass::Safe);
        match agent_resource(call) {
            Some(resource) => action.with_resource(resource),
            None => action,
        }
    }

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let agent = string_argument(call.arguments_json, "agent")?;
        let target = AgentId::from_wire(agent.trim()).map_err(|error| {
            tool_invoke_err(ToolError::invoke_rejected(format!(
                "invalid agent id '{agent}': {error}"
            )))
        })?;
        match self.context.get_subagent(target) {
            Some(snapshot) => Ok(ToolOutput {
                output: snapshot_json(&snapshot).to_string(),
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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::super::{subagent_tool_group, test_support::tool_named};
    use super::*;
    use crate::agent::graph::test_support::{host_with_tree, snap};
    use crate::agent::graph::{GraphHost, SpawnPolicy};

    #[test]
    fn watch_subagent_reports_snapshot_or_refuses() {
        let host = host_with_tree(vec![snap(1, None, 0), snap(2, Some(1), 1)]);
        let context =
            crate::agent::graph::test_support::context_for(host as Arc<dyn GraphHost>, AgentId(1));
        let watch = tool_named(
            &subagent_tool_group(context, SpawnPolicy::Any),
            "watch_subagent",
        );

        let ok = watch
            .invoke(&ToolInvocation {
                id: Some("w1"),
                name: "watch_subagent",
                arguments_json: r#"{"agent":"agent-2"}"#,
            })
            .unwrap();
        assert!(ok.ok);
        assert!(ok.output.contains("agent-2"));
        assert!(ok.output.contains("manual"));

        // Watching itself (not a descendant) is refused.
        let refused = watch
            .invoke(&ToolInvocation {
                id: Some("w2"),
                name: "watch_subagent",
                arguments_json: r#"{"agent":"agent-1"}"#,
            })
            .unwrap();
        assert!(!refused.ok);
    }
}
