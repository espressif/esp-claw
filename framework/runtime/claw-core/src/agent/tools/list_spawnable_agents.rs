//! `subagent.list_spawnable()` — the menu of subagent kinds this agent may spawn.
//!
//! A pure read of the agent's own [`SpawnPolicy`], rendered against the baked
//! manifests into `{kind, description}` rows. It exists so the model can *ask*
//! what it may spawn (and pick the right `kind` for `subagent.spawn`) instead of
//! guessing a kind and learning by rejection — and unlike baking the catalog into
//! `subagent.spawn`'s schema, it costs nothing in the always-sent prompt prefix.

use claw_tool::{
    tool_metadata, SyncToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use crate::agent::graph::SpawnPolicy;

/// Serves the spawnable-kinds catalog resolved from this agent's spawn policy.
pub(crate) struct ListSpawnableAgentsTool {
    pub(super) policy: SpawnPolicy,
}

impl ToolSpec for ListSpawnableAgentsTool {
    tool_metadata!("subagent.list_spawnable");

    // A pure read of static policy/manifest data — safe to run alongside others.
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
