use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use super::super::policy::SpawnPolicy;

pub(super) fn tool(policy: SpawnPolicy) -> Tool {
    Tool::from_sync(ListSpawnableAgentsTool { policy })
}

struct ListSpawnableAgentsTool {
    policy: SpawnPolicy,
}

impl ToolSpec for ListSpawnableAgentsTool {
    tool_metadata!("subagent_list_spawnable");

    fn concurrent(&self) -> bool {
        true
    }
}

impl SyncToolHandler for ListSpawnableAgentsTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let kinds: Vec<serde_json::Value> = self
            .policy
            .catalog()
            .iter()
            .map(|(kind, description)| {
                serde_json::json!({ "kind": kind.as_str(), "description": description })
            })
            .collect();
        Ok(ToolOutput {
            output: serde_json::json!({ "spawnable_agents": kinds }).to_string(),
            ok: true,
        })
    }
}
