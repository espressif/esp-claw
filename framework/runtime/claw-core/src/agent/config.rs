//! Runnable agent config assembled by the factory.
//!
//! A baked manifest is compile-time data: prompt, tool names, skill ids, and
//! spawn policy. Runtime binding belongs to `FsAgentFactory`; this module only
//! stores the resolved config passed to `GenericAgent`.

use claw_api::RetryPolicy;
use claw_skill::SkillSet;
use claw_tool::Tool;

use crate::agent::graph::SpawnPolicy;
use crate::agent::kind::AgentKind;
use crate::agent::manifest::{AgentManifest, RetryCount};

/// A fully-resolved agent configuration.
///
/// The factory consumes local tools from this config into a `ToolSet` before
/// constructing the generic agent.
pub struct AgentConfig {
    pub(in crate::agent) kind: AgentKind,
    pub(in crate::agent) system_prompt: String,
    pub(in crate::agent) tools: Vec<Tool>,
    pub(in crate::agent) skills: SkillSet,
    /// Whether this kind may spawn subagents.
    pub(in crate::agent) spawn_enabled: bool,
    /// The kinds this agent may spawn.
    pub(in crate::agent) spawn_policy: SpawnPolicy,
    pub(in crate::agent) retry_policy: RetryPolicy,
    pub(in crate::agent) tool_block_retries: RetryCount,
}

impl AgentConfig {
    pub(in crate::agent) fn from_manifest(
        manifest: &'static AgentManifest,
        tools: Vec<Tool>,
        skills: SkillSet,
    ) -> Self {
        Self {
            kind: manifest.kind.clone(),
            system_prompt: manifest.instructions.trim().to_string(),
            tools,
            skills,
            spawn_enabled: manifest.spawn_enabled,
            spawn_policy: SpawnPolicy::from_allowed_kinds(manifest.allowed_kinds),
            retry_policy: RetryPolicy::new(manifest.retries.get()),
            tool_block_retries: manifest.tool_block_retries,
        }
    }

    /// This config's kind.
    pub fn kind(&self) -> &AgentKind {
        &self.kind
    }
}

/// Failure resolving baked manifest data into an [`AgentConfig`].
#[derive(Clone, Debug, thiserror::Error)]
pub enum AgentConfigError {
    /// No manifest is baked into the firmware for the requested kind.
    #[error("unknown agent kind: {0}")]
    UnknownKind(String),
    /// A tool name in the manifest has no local binding.
    #[error("unknown tool: {0}")]
    UnknownTool(String),
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;
    use crate::agent::manifest::MANIFESTS;

    #[test]
    fn every_baked_kind_builds_static_config() {
        for manifest in MANIFESTS {
            let kind = manifest.kind.as_str();
            let config = AgentConfig::from_manifest(manifest, Vec::new(), SkillSet::empty());
            assert_eq!(config.kind().as_str(), kind);
            assert!(
                !config.system_prompt.is_empty(),
                "kind {kind} has no prompt"
            );
        }
    }
}
