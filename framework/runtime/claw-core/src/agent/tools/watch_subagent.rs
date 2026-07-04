//! `watch_subagent(agent)` — snapshot one descendant by id.

use std::sync::Arc;

use claw_capability::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};
use claw_permission::{Action, RiskClass};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::AgentContext;

use super::{agent_resource, string_argument};

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

impl ToolSpec for WatchSubagentTool {
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
}

impl SyncToolHandler for WatchSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let agent = string_argument(call.arguments_json(), "agent")?;
        let target = AgentId::from_wire(agent.trim()).map_err(|error| {
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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::agent::graph::test_support::{host_with_tree, snap};
    use crate::agent::graph::GraphHost;
    use claw_capability::RawToolInvocation;

    fn call<'a>(id: &'a str, arguments_json: &'a str) -> ToolInvocation<'a> {
        ToolInvocation::try_from(RawToolInvocation {
            id: Some(id),
            name: "watch_subagent",
            arguments_json,
        })
        .unwrap()
    }

    #[test]
    fn watch_subagent_reports_snapshot_or_refuses() {
        let host = host_with_tree(vec![snap(1, None, 0), snap(2, Some(1), 1)]);
        let context =
            crate::agent::graph::test_support::context_for(host as Arc<dyn GraphHost>, AgentId(1));
        let watch = WatchSubagentTool::new(context);

        let ok = watch.invoke(&call("w1", r#"{"agent":"agent-2"}"#)).unwrap();
        assert!(ok.ok);
        assert!(ok.output.contains("agent-2"));
        assert!(ok.output.contains("manual"));

        // Watching itself (not a descendant) is refused.
        let refused = watch.invoke(&call("w2", r#"{"agent":"agent-1"}"#)).unwrap();
        assert!(!refused.ok);
    }
}
