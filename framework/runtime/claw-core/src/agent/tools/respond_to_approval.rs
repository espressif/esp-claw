//! `respond_to_approval(agent, verdict, note)` — the root reports its
//! classification of a user's reply to a subagent's approval request.
//!
//! Only the session root talks to the user, so any agent's pending approval (an
//! `Ask` decision raised by its permission policy) bubbles up to the root (done by
//! the orchestrator instance). The root presents it, reads the user's free-text
//! reply, classifies it into yes/no/other, and reports the verdict back here. Like
//! `spawn_subagent`, this affects *another* agent, so it emits a
//! [`GraphEffect::ResolveApproval`](crate::agent::graph::GraphEffect::ResolveApproval)
//! through the agent's [`AgentContext`] for the instance to apply at a borrow-safe
//! point rather than touching the graph here.

use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, ToolError, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput,
};

use crate::agent::base_agent::AgentId;
use crate::agent::graph::{AgentContext, ApprovalVerdict};

use super::{agent_resource, string_argument};

/// Reports the root's verdict on a pending approval through the graph context.
pub(crate) struct RespondToApprovalTool {
    context: Arc<AgentContext>,
}

impl RespondToApprovalTool {
    /// Build the tool over the agent's `context`.
    pub(crate) fn new(context: Arc<AgentContext>) -> Self {
        Self { context }
    }
}

impl ToolHandler for RespondToApprovalTool {
    tool_metadata!("respond_to_approval");

    fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        let action = Action::new("respond_to_approval", RiskClass::Low);
        match agent_resource(call) {
            Some(resource) => action.with_resource(resource),
            None => action,
        }
    }

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let agent = string_argument(call.arguments_json, "agent")?;
        let target = AgentId::from_wire(agent.trim()).map_err(|error| {
            ToolError::invoke_rejected(format!("invalid agent id '{agent}': {error}"))
        })?;

        let verdict_raw = string_argument(call.arguments_json, "verdict")?;
        let verdict = match verdict_raw.trim() {
            "yes" => ApprovalVerdict::Yes,
            "no" => ApprovalVerdict::No,
            "other" => ApprovalVerdict::Other,
            other => {
                return Err(ToolError::invoke_rejected(format!(
                    "respond_to_approval 'verdict' must be one of yes|no|other, got '{other}'"
                ))
                .into())
            }
        };

        let note_raw = string_argument(call.arguments_json, "note")?;
        let note = (!note_raw.trim().is_empty()).then_some(note_raw);

        self.context.respond_to_approval(target, verdict, note);
        Ok(ToolOutput {
            output: format!("Recorded '{verdict_raw}' for {target}."),
            ok: true,
        })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::super::respond_to_approval_tool_group;
    use super::*;
    use crate::agent::graph::test_support::{context_for, RecordingHost};
    use crate::agent::graph::{GraphEffect, GraphHost};

    #[test]
    fn respond_to_approval_parses_target_and_verdict() {
        let host = Arc::new(RecordingHost::default());
        let context = context_for(Arc::clone(&host) as Arc<dyn GraphHost>, AgentId(1));

        let group = respond_to_approval_tool_group(context);
        let tool = group
            .tools()
            .iter()
            .find(|tool| tool.name() == "respond_to_approval")
            .unwrap()
            .clone();

        tool.invoke(&ToolInvocation {
            id: Some("t1"),
            name: "respond_to_approval",
            arguments_json: r#"{"agent":"agent-7","verdict":"no","note":"not allowed"}"#,
        })
        .unwrap();

        let effects = host.effects.lock().unwrap();
        assert_eq!(
            effects.as_slice(),
            &[(
                AgentId(1),
                GraphEffect::ResolveApproval {
                    target: AgentId(7),
                    verdict: ApprovalVerdict::No,
                    note: Some("not allowed".to_string()),
                }
            )]
        );
    }

    #[test]
    fn respond_to_approval_rejects_unknown_verdict() {
        let host = Arc::new(RecordingHost::default());
        let context = context_for(host as Arc<dyn GraphHost>, AgentId(1));
        let group = respond_to_approval_tool_group(context);
        let tool = group.tools().iter().next().unwrap().clone();

        let error = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "respond_to_approval",
                arguments_json: r#"{"agent":"agent-1","verdict":"maybe"}"#,
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
        assert!(error.retries.is_none());
    }
}
