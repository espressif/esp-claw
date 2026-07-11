//! Agent manifests: the compile-time-baked, typed **data** for every agent kind.
//!
//! This module is *only* data. What distinguishes one agent "kind" from another
//! is a system prompt plus the names of the tools/skills it may use, defined on
//! disk under `resources/agents/<kind>/`. The build script (see
//! `manifest_gen/`) parses and **validates each kind at compile time** and emits
//! one [`AgentManifest`] per kind into the [`MANIFESTS`] array (`include!`-d
//! below), so malformed metadata fails the build, not the device, and nothing is
//! parsed at runtime.
//!
//! Turning a manifest into something runnable is a factory concern. This module
//! knows nothing about resolution, tools, or the running agent.

use claw_skill::SkillId;

use crate::agent::kind::AgentKind;

/// A retry budget: a count of extra attempts.
///
/// Newtype over `u32` so a retry count can't be silently swapped with an
/// unrelated integer at a call site, and so its meaning is explicit in the
/// manifest. Constructed in build-script-generated code via [`RetryCount::new`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RetryCount(u32);

impl RetryCount {
    /// Wrap a raw retry count.
    pub const fn new(count: u32) -> Self {
        Self(count)
    }

    /// The underlying count.
    pub const fn get(self) -> u32 {
        self.0
    }
}

/// One agent kind's compile-time-baked definition: the system prompt plus the
/// validated metadata and the kind's tool/skill lists, as their domain types.
///
/// Names are baked as their typed forms ([`AgentKind`], [`SkillId`],
/// tool group ids) — each backed by a `&'static str` so the whole value lives
/// in a `const`. This is pure data; binding the tool/skill names to handler
/// *code* happens elsewhere, at runtime.
#[derive(Clone, Debug)]
pub struct AgentManifest {
    /// The kind/role this manifest defines (matches its directory name).
    pub kind: AgentKind,
    /// Human/model-facing summary of the kind's purpose (`agent.json`).
    pub description: &'static str,
    /// Whether this kind may spawn subagents (`spawn.enabled`).
    pub spawn_enabled: bool,
    /// Intended allowlist of kinds this agent may spawn (`spawn.allowed_kinds`;
    /// `"*"` means any). Declarative — not yet enforced at runtime.
    pub allowed_kinds: &'static [AgentKind],
    /// LLM retry budget per iteration (`runtime.retries`).
    pub retries: RetryCount,
    /// Consecutive gating-blocked tool rounds to tolerate
    /// (`runtime.tool_block_retries`; defaults to 0 in the build-time parser).
    pub tool_block_retries: RetryCount,
    /// Registry tool groups this kind may use.
    pub tool_groups: &'static [&'static str],
    /// Skill ids this kind loads, resolved to a skill set at runtime.
    pub skills: &'static [SkillId],
    /// `instructions.md` — the agent's persona/process guidance (system prompt).
    pub instructions: &'static str,
}

impl AgentManifest {
    /// The baked manifest for `kind`, or `None` if no such kind exists.
    pub fn for_kind(kind: &str) -> Option<&'static AgentManifest> {
        MANIFESTS
            .iter()
            .find(|manifest| manifest.kind.as_str() == kind)
    }
}

// The build script emits `pub const MANIFESTS: &[AgentManifest]` — one
// entry per kind under `resources/agents/`. This `include!` must follow the
// `AgentManifest` definition (and the field types) the generated code references.
include!(concat!(env!("OUT_DIR"), "/manifests.rs"));
