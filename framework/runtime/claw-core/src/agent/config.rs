//! Runnable agent config assembled by the factory.
//!
//! A baked manifest is compile-time data: prompt, tool names, skill ids, and
//! spawn policy. Runtime binding belongs to `FsAgentFactory`; this module only
//! stores the resolved config consumed at the factory's private assembly point.

use claw_api::RetryPolicy;
use claw_skill::SkillSet;

use crate::agent::graph::SpawnPolicy;
use crate::agent::manifest::AgentManifest;

/// A fully-resolved agent configuration.
///
pub(super) struct AgentConfig {
    pub(in crate::agent) system_prompt: String,
    pub(in crate::agent) skills: SkillSet,
    /// Whether this kind may spawn subagents.
    pub(in crate::agent) spawn_enabled: bool,
    /// The kinds this agent may spawn.
    pub(in crate::agent) spawn_policy: SpawnPolicy,
    pub(in crate::agent) retry_policy: RetryPolicy,
    pub(in crate::agent) tool_block_retries: u32,
}

impl AgentConfig {
    pub(in crate::agent) fn from_manifest(
        manifest: &'static AgentManifest,
        skills: SkillSet,
    ) -> Self {
        Self {
            system_prompt: manifest.instructions.trim().to_string(),
            skills,
            spawn_enabled: manifest.spawn_enabled,
            spawn_policy: SpawnPolicy::from_allowed_kinds(manifest.allowed_kinds),
            retry_policy: RetryPolicy::new(manifest.retries),
            tool_block_retries: manifest.tool_block_retries,
        }
    }
}

/// Failure resolving baked manifest data into an [`AgentConfig`].
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentConfigError {
    /// No manifest is baked into the firmware for the requested kind.
    #[error("unknown agent kind: {0}")]
    UnknownKind(String),
}
