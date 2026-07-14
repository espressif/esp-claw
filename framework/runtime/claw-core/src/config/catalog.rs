//! Compile-time catalog shared by the single-agent factory and orchestrator.
//!
//! The catalog is data, not runtime ownership. [`AgentRuntimeManifest`] is the
//! projection consumed by `agent`; [`MultiagentManifest`] is consumed only by
//! the orchestrator's multiagent extension.

use claw_skill::SkillId;

use crate::protocol::AgentKind;

pub(crate) struct AgentCatalogEntry {
    kind: AgentKind,
    description: &'static str,
    runtime: AgentRuntimeManifest,
    multiagent: MultiagentManifest,
}

impl AgentCatalogEntry {
    pub(crate) fn kind(&self) -> &AgentKind {
        &self.kind
    }

    pub(crate) fn description(&self) -> &'static str {
        self.description
    }

    pub(crate) fn runtime(&self) -> &AgentRuntimeManifest {
        &self.runtime
    }

    pub(crate) fn multiagent(&self) -> &MultiagentManifest {
        &self.multiagent
    }
}

/// Configuration needed to construct one agent in isolation.
pub(crate) struct AgentRuntimeManifest {
    retries: u32,
    tool_block_retries: u32,
    tool_groups: &'static [&'static str],
    skills: &'static [SkillId],
    instructions: &'static str,
}

impl AgentRuntimeManifest {
    pub(crate) fn retries(&self) -> u32 {
        self.retries
    }

    pub(crate) fn tool_block_retries(&self) -> u32 {
        self.tool_block_retries
    }

    pub(crate) fn tool_groups(&self) -> &'static [&'static str] {
        self.tool_groups
    }

    pub(crate) fn skills(&self) -> &'static [SkillId] {
        self.skills
    }

    pub(crate) fn instructions(&self) -> &'static str {
        self.instructions
    }
}

/// Orchestration policy intentionally kept outside the single-agent runtime.
pub(crate) struct MultiagentManifest {
    spawn_enabled: bool,
    allowed_kinds: &'static [AgentKind],
}

impl MultiagentManifest {
    pub(crate) fn spawn_enabled(&self) -> bool {
        self.spawn_enabled
    }

    pub(crate) fn allowed_kinds(&self) -> &'static [AgentKind] {
        self.allowed_kinds
    }
}

pub(crate) fn find(kind: &AgentKind) -> Option<&'static AgentCatalogEntry> {
    entries().iter().find(|entry| entry.kind() == kind)
}

pub(crate) fn entries() -> &'static [AgentCatalogEntry] {
    ENTRIES
}

include!(concat!(env!("OUT_DIR"), "/manifests.rs"));
