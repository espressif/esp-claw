//! Resolving a baked agent manifest into a runnable [`AgentConfig`].
//!
//! # Relationship to the manifest
//!
//! An agent manifest is pure compile-time
//! **data**: a system prompt plus the *names* of capabilities/skills. The actual
//! tool/skill *handlers* are code that lives in firmware (or a test double), so a
//! manifest cannot run on its own. This module is the **seam** that binds the two:
//!
//! ```text
//!   AgentManifest (data: names)  +  AgentResolver (names -> handler code)
//!         └──────────────── AgentConfig::resolve(kind, ...) ───────────────┘
//!                                     │
//!                                     ▼
//!                              AgentConfig (runnable)
//!                                     │
//!                                     ▼
//!            FsAgentFactory builds ToolSet, then GenericAgent::new(...)
//! ```
//!
//! - [`AgentResolver`] is the injected boundary that turns a capability/skill
//!   *name* into a real [`Capability`](claw_capability::Capability) / [`SkillSet`];
//!   `claw-core` then decomposes the capability into its internal `Tool`.
//! - [`AgentConfig`] is the resolved manifest data. The factory consumes its
//!   local tools into a `ToolSet` before constructing [`GenericAgent`].

use claw_api::RetryPolicy;
use claw_skill::{SkillError, SkillSet};

use crate::agent::graph::SpawnPolicy;
use crate::agent::kind::AgentKind;
use crate::agent::manifest::{AgentManifest, RetryCount};
use crate::agent::resolver::AgentResolver;
use claw_capability::{Capability, Tool};

/// A fully-resolved agent configuration — the typed seam between a baked manifest
/// and the agent factory that builds it. The factory consumes local tools from
/// this config into a `ToolSet` before constructing the generic agent.
///
/// The only way to build one is [`AgentConfig::resolve`]: every config originates
/// from a compile-time-baked manifest, so there is no hand-rolled builder. The
/// fields are `pub(in crate::agent)` rather than fully public: only the runtime
/// that consumes a config ([`GenericAgent`](crate::agent::GenericAgent), a sibling
/// module) reads them.
pub struct AgentConfig {
    pub(in crate::agent) kind: AgentKind,
    pub(in crate::agent) system_prompt: String,
    pub(in crate::agent) tools: Vec<Tool>,
    pub(in crate::agent) skills: SkillSet,
    /// Whether this kind may spawn subagents (the manifest's `spawn.enabled`). The
    /// `spawn_subagent` tool is attached only when this is set *and* the agent has
    /// a graph host.
    pub(in crate::agent) spawn_enabled: bool,
    /// The kinds this agent may spawn (resolved from the manifest's
    /// `spawn.allowed_kinds`). Enforced by the `spawn_subagent` tool; meaningful
    /// only when `spawn_enabled`.
    pub(in crate::agent) spawn_policy: SpawnPolicy,
    pub(in crate::agent) retry_policy: RetryPolicy,
    pub(in crate::agent) tool_block_retries: RetryCount,
}

impl AgentConfig {
    /// Resolve a firmware-baked agent kind into a runnable config.
    ///
    /// Looks up the compile-time manifest baked for `kind` and resolves
    /// every capability/skill *name* in it through `resolver` into handler code.
    /// The manifest's JSON was already parsed and validated at build time, so this
    /// does only the runtime-only half: turning names into handlers.
    ///
    /// The result is pure data: local manifest tools are consumed by the factory,
    /// while graph tools are attached later from `spawn_enabled`.
    ///
    /// # Errors
    ///
    /// - [`AgentConfigError::UnknownKind`] if no manifest is baked for `kind`.
    /// - [`AgentConfigError::UnknownCapability`] if a capability name has no
    ///   handler in the resolver.
    /// - [`AgentConfigError::Skill`] if building the skill set fails (e.g. an
    ///   unknown skill id).
    pub fn resolve(
        kind: &str,
        resolver: &dyn AgentResolver,
    ) -> Result<AgentConfig, AgentConfigError> {
        let manifest = AgentManifest::for_kind(kind)
            .ok_or_else(|| AgentConfigError::UnknownKind(kind.to_string()))?;

        let mut tools = Vec::with_capacity(manifest.capabilities.len());
        for capability_name in manifest.capabilities {
            let name = capability_name.as_str();
            // The resolver speaks the `Capability` vocabulary; `claw-core`
            // decomposes it into the internal `Tool`. A missing name or a
            // resolved non-tool capability is a config error (never dropped).
            let capability = resolver
                .resolve_capability(name)
                .ok_or_else(|| AgentConfigError::UnknownCapability(name.to_string()))?;
            match capability {
                Capability::Tool(tool) => tools.push(tool),
                Capability::Channel(_) => {
                    return Err(AgentConfigError::UnsupportedCapability(name.to_string()));
                }
            }
        }

        let skills = resolver.build_skills(manifest.skills)?;

        Ok(AgentConfig {
            kind: manifest.kind.clone(),
            system_prompt: manifest.instructions.trim().to_string(),
            tools,
            skills,
            spawn_enabled: manifest.spawn_enabled,
            spawn_policy: SpawnPolicy::from_allowed_kinds(manifest.allowed_kinds),
            retry_policy: RetryPolicy::new(manifest.retries.get()),
            tool_block_retries: manifest.tool_block_retries,
        })
    }

    /// This config's kind.
    pub fn kind(&self) -> &AgentKind {
        &self.kind
    }
}

/// Failure resolving a baked agent kind into an [`AgentConfig`].
///
/// The manifest JSON is parsed and validated at build time, so the failures here
/// are an unknown kind or runtime resolution misses against the injected
/// [`AgentResolver`].
#[derive(Clone, Debug, thiserror::Error)]
pub enum AgentConfigError {
    /// No manifest is baked into the firmware for the requested kind.
    #[error("unknown agent kind: {0}")]
    UnknownKind(String),
    /// A capability name in the manifest has no handler in the resolver.
    #[error("unknown capability: {0}")]
    UnknownCapability(String),
    /// A manifest capability resolved to a non-tool capability.
    #[error("unsupported capability: {0}")]
    UnsupportedCapability(String),
    /// Building the skill set failed (e.g. an unknown skill id).
    #[error(transparent)]
    Skill(#[from] SkillError),
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;
    use crate::agent::manifest::MANIFESTS;
    use claw_capability::Capability;
    use claw_skill::SkillId;

    /// A test resolver that maps names to no tools and supports no skills.
    struct StaticResolver;

    impl AgentResolver for StaticResolver {
        fn resolve_capability(&self, _name: &str) -> Option<Capability> {
            None
        }
        fn build_skills(&self, _skill_ids: &[SkillId]) -> Result<SkillSet, SkillError> {
            Ok(SkillSet::empty())
        }
    }

    #[test]
    fn every_baked_kind_resolves() {
        for manifest in MANIFESTS {
            let kind = manifest.kind.as_str();
            let config = AgentConfig::resolve(kind, &StaticResolver)
                .unwrap_or_else(|error| panic!("kind {kind} failed: {error}"));
            assert_eq!(config.kind().as_str(), kind);
            assert!(
                !config.system_prompt.is_empty(),
                "kind {kind} has no prompt"
            );
        }
    }

    #[test]
    fn resolve_rejects_an_unknown_kind() {
        // `AgentConfig` is not `Debug` (it holds tools/skills/Arcs), so match on
        // the `Result` directly rather than `expect_err`.
        let result = AgentConfig::resolve("nope", &StaticResolver);
        assert!(matches!(result, Err(AgentConfigError::UnknownKind(kind)) if kind == "nope"));
    }
}
