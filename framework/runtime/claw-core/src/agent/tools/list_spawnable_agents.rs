//! `list_spawnable_agents()` — the menu of subagent kinds this agent may spawn.
//!
//! A pure read of the agent's own [`SpawnPolicy`], rendered against the baked
//! manifests into `{kind, description}` rows. It exists so the model can *ask*
//! what it may spawn (and pick the right `kind` for `spawn_subagent`) instead of
//! guessing a kind and learning by rejection — and unlike baking the catalog into
//! `spawn_subagent`'s schema, it costs nothing in the always-sent prompt prefix.

use claw_capability::{
    tool_metadata, SyncToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolSpec,
};

use crate::agent::graph::SpawnPolicy;

/// Serves the spawnable-kinds catalog resolved from this agent's spawn policy.
pub(crate) struct ListSpawnableAgentsTool {
    policy: SpawnPolicy,
}

impl ListSpawnableAgentsTool {
    /// Build the tool over the agent's spawn `policy` (the same policy
    /// `spawn_subagent` enforces, so the menu and the gate never disagree).
    pub(crate) fn new(policy: SpawnPolicy) -> Self {
        Self { policy }
    }
}

impl ToolSpec for ListSpawnableAgentsTool {
    tool_metadata!("list_spawnable_agents");

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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::agent::kind::AgentKind;
    use claw_capability::RawToolInvocation;

    fn call() -> ToolInvocation<'static> {
        ToolInvocation::try_from(RawToolInvocation {
            id: Some("t1"),
            name: "list_spawnable_agents",
            arguments_json: "{}",
        })
        .unwrap()
    }

    fn spawnable(policy: SpawnPolicy) -> serde_json::Value {
        let tool = ListSpawnableAgentsTool::new(policy);
        let output = tool.invoke(&call()).unwrap();
        assert!(output.ok);
        serde_json::from_str(&output.output).unwrap()
    }

    #[test]
    fn only_policy_lists_the_allowed_kind_with_its_description() {
        let value = spawnable(SpawnPolicy::Only(vec![AgentKind::new("worker")]));
        let rows = value["spawnable_agents"].as_array().unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0]["kind"], "worker");
        assert!(rows[0]["description"]
            .as_str()
            .unwrap()
            .contains("Executes one delegated task"));
    }

    #[test]
    fn any_policy_lists_every_baked_kind() {
        let value = spawnable(SpawnPolicy::Any);
        let kinds: Vec<&str> = value["spawnable_agents"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|row| row["kind"].as_str())
            .collect();
        assert!(kinds.contains(&"conversation"));
        assert!(kinds.contains(&"worker"));
    }

    #[test]
    fn unknown_only_kind_is_dropped() {
        // A kind with no baked manifest can never be built, so it must not be
        // offered as spawnable.
        let value = spawnable(SpawnPolicy::Only(vec![AgentKind::new("ghost")]));
        assert!(value["spawnable_agents"].as_array().unwrap().is_empty());
    }
}
