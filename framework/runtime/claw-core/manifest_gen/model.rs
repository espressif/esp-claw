//! Serde shapes for the on-disk agent manifest JSON, used only by the build
//! script. These mirror the files under `resources/agents/<kind>/` and are
//! deserialized in [`crate::parse`].

use serde::Deserialize;

/// `agent.json` — the kind's metadata header.
#[derive(Debug, Deserialize)]
pub struct AgentJson {
    /// Schema version of `agent.json`; reserved for forward-compatibility.
    #[allow(dead_code)]
    pub schema_version: u32,
    /// The kind/role this directory defines (validated against the dir name).
    pub kind: String,
    /// Human/model-facing summary of the kind's purpose.
    pub description: String,
    /// Whether this kind may spawn subagents.
    pub spawn: SpawnJson,
    /// Per-agent runtime tuning.
    pub runtime: RuntimeJson,
}

/// The `spawn` block of `agent.json`.
#[derive(Debug, Deserialize)]
pub struct SpawnJson {
    /// Gates the `subagent.spawn` tool.
    pub enabled: bool,
    /// Intended allowlist of kinds this agent may spawn (`"*"` = any).
    pub allowed_kinds: Vec<String>,
}

/// The `runtime` block of `agent.json`.
#[derive(Debug, Deserialize)]
pub struct RuntimeJson {
    /// LLM retry count per iteration.
    pub retries: u32,
    /// Consecutive gating-blocked tool rounds to tolerate.
    #[serde(default)]
    pub tool_block_retries: u32,
}

/// `tools/tools.json` — the tool names this kind uses.
#[derive(Debug, Deserialize)]
pub struct ToolsJson {
    /// Schema version; reserved for forward-compatibility.
    #[allow(dead_code)]
    pub schema_version: u32,
    /// Tool names resolved to handlers at runtime.
    pub tools: Vec<String>,
}

/// `skills/skills.json` — the skill ids this kind loads.
#[derive(Debug, Deserialize)]
pub struct SkillsJson {
    /// Schema version; reserved for forward-compatibility.
    #[allow(dead_code)]
    pub schema_version: u32,
    /// Skill ids loaded into the agent's skill set at runtime.
    pub skills: Vec<String>,
}
