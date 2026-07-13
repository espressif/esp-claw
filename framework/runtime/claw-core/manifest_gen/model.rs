//! Serde shapes for the on-disk agent manifest JSON, used only by the build
//! script. These mirror the files under `resources/agents/<kind>/` and are
//! deserialized in [`crate::parse`].

use serde::Deserialize;

/// `agent.json` — the kind's metadata header.
#[derive(Debug, Deserialize)]
pub(crate) struct AgentJson {
    /// Schema version of `agent.json`; reserved for forward-compatibility.
    #[allow(dead_code)]
    pub(crate) schema_version: u32,
    /// The kind/role this directory defines (validated against the dir name).
    pub(crate) kind: String,
    /// Human/model-facing summary of the kind's purpose.
    pub(crate) description: String,
    /// Whether this kind may spawn subagents.
    pub(crate) spawn: SpawnJson,
    /// Per-agent runtime tuning.
    pub(crate) runtime: RuntimeJson,
}

/// The `spawn` block of `agent.json`.
#[derive(Debug, Deserialize)]
pub(crate) struct SpawnJson {
    /// Gates the `subagent_spawn` tool.
    pub(crate) enabled: bool,
    /// Intended allowlist of kinds this agent may spawn (`"*"` = any).
    pub(crate) allowed_kinds: Vec<String>,
}

/// The `runtime` block of `agent.json`.
#[derive(Debug, Deserialize)]
pub(crate) struct RuntimeJson {
    /// LLM retry count per iteration.
    pub(crate) retries: u32,
    /// Consecutive gating-blocked tool rounds to tolerate.
    #[serde(default)]
    pub(crate) tool_block_retries: u32,
}

/// `tools/tools.json` — the tool groups this kind may use.
#[derive(Debug, Deserialize)]
pub(crate) struct ToolsJson {
    /// Schema version; reserved for forward-compatibility.
    #[allow(dead_code)]
    pub(crate) schema_version: u32,
    /// Tool group ids allowed for this agent kind.
    pub(crate) tool_groups: Vec<String>,
}

/// `skills/skills.json` — the skill ids this kind loads.
#[derive(Debug, Deserialize)]
pub(crate) struct SkillsJson {
    /// Schema version; reserved for forward-compatibility.
    #[allow(dead_code)]
    pub(crate) schema_version: u32,
    /// Skill ids loaded into the agent's skill set at runtime.
    pub(crate) skills: Vec<String>,
}
