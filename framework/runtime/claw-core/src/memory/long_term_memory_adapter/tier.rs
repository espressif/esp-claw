//! Memory tiering: routing a fact to the **global** (shared across all agents)
//! or **agent** (private to one agent) long-term store.
//!
//! Tiering is an implementation detail the agent must never expose to the model:
//! the memory tools take no `scope`/`tier` parameter, so every new fact is routed
//! deterministically from its tags.

use claw_memory::MemoryDraft;

/// Which long-term store a fact belongs to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum MemoryTier {
    /// Shared across every agent and session (e.g. user profile, identity).
    Global,
    /// Private to the agent that stored it (e.g. a task-specific note).
    Agent,
}

/// Tags that mark a fact as globally shared (user-level, not
/// task-level). Persona/profile/assistant identity changes are intentionally not
/// here; those belong to the profile documents and tools.
const DEFAULT_GLOBAL_TAGS: &[&str] = &["preference", "user", "device", "fact", "shared"];

/// Route to global memory when any tag is in the shared set; otherwise keep the
/// fact private to the agent.
pub(super) fn classify_tier(draft: &MemoryDraft) -> MemoryTier {
    let is_global = draft.tags.iter().any(|tag| {
        DEFAULT_GLOBAL_TAGS
            .iter()
            .any(|known| known.eq_ignore_ascii_case(tag))
    });
    if is_global {
        MemoryTier::Global
    } else {
        MemoryTier::Agent
    }
}

#[cfg(test)]
mod tests {
    use claw_memory::MemoryDraft;

    use super::{classify_tier, MemoryTier};

    #[test]
    fn shared_tag_routes_to_global_memory() {
        let draft = MemoryDraft::new("Uses Home Assistant").with_tags(["Preference".into()]);

        assert_eq!(classify_tier(&draft), MemoryTier::Global);
    }

    #[test]
    fn task_tag_routes_to_agent_memory() {
        let draft = MemoryDraft::new("Deploy step needs sudo").with_tags(["task".into()]);

        assert_eq!(classify_tier(&draft), MemoryTier::Agent);
    }
}
