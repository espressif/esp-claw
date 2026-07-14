use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, AsyncToolHandler, Tool, ToolError, ToolFuture, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolSpec,
};

use crate::protocol::{AgentKind, Message};

use super::super::model::TranscriptText;
use super::super::policy::SpawnPolicy;
use super::super::tool_port::SubagentControl;
use super::args::{non_blank_argument, required_bool_argument};

pub(super) fn tool(control: Arc<SubagentControl>, policy: SpawnPolicy) -> Tool {
    Tool::from_async(SpawnSubagentTool { control, policy })
}

struct SpawnSubagentTool {
    control: Arc<SubagentControl>,
    policy: SpawnPolicy,
}

impl ToolSpec for SpawnSubagentTool {
    tool_metadata!("subagent_spawn");

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new("subagent_spawn", RiskClass::Moderate)
    }
}

impl AsyncToolHandler for SpawnSubagentTool {
    fn invoke<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a> {
        Box::pin(async move { self.invoke_inner(call).await })
    }
}

impl SpawnSubagentTool {
    async fn invoke_inner(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let kind = AgentKind::new(non_blank_argument(call.arguments_json(), "kind")?);
        if !self.policy.allows(&kind) {
            tracing::warn!(name: "spawn_kind_rejected", kind = %kind.as_str());
            return Ok(ToolOutput {
                output: format!(
                    "subagent_spawn: kind '{kind}' is not permitted for this agent. \
                     Allowed: {}. This is a policy restriction, not a transient error: \
                     pick a permitted kind or handle the work yourself.",
                    self.policy.describe()
                ),
                ok: false,
            });
        }

        if !SpawnPolicy::is_known(&kind) {
            tracing::warn!(name: "spawn_unknown_kind_rejected", kind = %kind.as_str());
            let available = self
                .policy
                .catalog()
                .iter()
                .map(|(agent_kind, _)| agent_kind.as_str())
                .collect::<Vec<_>>()
                .join(", ");
            let available = if available.is_empty() {
                "(none)".to_string()
            } else {
                available
            };
            return Ok(ToolOutput {
                output: format!(
                    "subagent_spawn: '{kind}' is not a known agent kind, so it cannot be \
                     created. Spawnable kinds: {available}. Call subagent_list_spawnable to see \
                     what you can spawn."
                ),
                ok: false,
            });
        }

        let name = non_blank_argument(call.arguments_json(), "name")?;
        let goal = Message::text(non_blank_argument(call.arguments_json(), "goal")?);
        let foreground = required_bool_argument(call.arguments_json(), "foreground")?;
        if foreground {
            let (_child, result) = self.control.spawn_foreground(kind, Some(name), goal);
            let result = result.recv().await.map_err(|_| {
                ToolError::InvokeRejected("foreground subagent result channel closed".to_owned())
            })?;
            Ok(ToolOutput {
                output: result.text(),
                ok: result.ok(),
            })
        } else {
            let child = self
                .control
                .spawn_background(kind, Some(name.clone()), goal);
            Ok(ToolOutput {
                output: format!(
                    "Subagent {child} named '{name}' requested; its result will be reported back when it finishes."
                ),
                ok: true,
            })
        }
    }
}
