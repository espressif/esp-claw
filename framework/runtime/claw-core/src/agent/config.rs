//! Runnable agent config assembled by the factory.
//!
//! Runtime binding belongs to `FsAgentFactory`; this module only stores the
//! single-agent config consumed at the factory's private assembly point.

use claw_api::RetryPolicy;
use claw_skill::SkillSet;

use crate::config::catalog::AgentRuntimeManifest;

/// A fully-resolved agent configuration.
///
pub(super) struct AgentConfig {
    pub(in crate::agent) system_prompt: String,
    pub(in crate::agent) skills: SkillSet,
    pub(in crate::agent) tool_groups: &'static [&'static str],
    pub(in crate::agent) retry_policy: RetryPolicy,
    pub(in crate::agent) tool_block_retries: u32,
}

impl AgentConfig {
    pub(in crate::agent) fn from_manifest(
        manifest: &'static AgentRuntimeManifest,
        skills: SkillSet,
    ) -> Self {
        Self {
            system_prompt: manifest.instructions().trim().to_string(),
            skills,
            tool_groups: manifest.tool_groups(),
            retry_policy: RetryPolicy::new(manifest.retries()),
            tool_block_retries: manifest.tool_block_retries(),
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
