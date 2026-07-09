//! Memory tiering: routing a fact to the **global** (shared across all agents)
//! or **agent** (private to one agent) long-term store.
//!
//! Tiering is a policy the agent must never expose to the model: the memory
//! tools take no `scope`/`tier` parameter, so the LLM cannot be misdirected into
//! choosing a store. Instead every new fact passes through a [`TierClassifier`]
//! that decides deterministically from the fact's tags.

use std::sync::Arc;

use claw_memory::MemoryDraft;

/// Which long-term store a fact belongs to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemoryTier {
    /// Shared across every agent and session (e.g. user profile, identity).
    Global,
    /// Private to the agent that stored it (e.g. a task-specific note).
    Agent,
}

/// Routes a new fact to a [`MemoryTier`].
///
/// Kept injectable so the routing policy can be swapped without touching the
/// adapter.
pub trait TierClassifier: Send + Sync {
    /// Decide the tier for `draft`.
    fn classify(&self, draft: &MemoryDraft) -> MemoryTier;
}

/// Tags that, by default, mark a fact as globally shared (user-level, not
/// task-level). Persona/profile/assistant identity changes are intentionally not
/// here; those belong to the profile documents and tools.
const DEFAULT_GLOBAL_TAGS: &[&str] = &["preference", "user", "device", "fact", "shared"];

/// The default [`TierClassifier`]: route to
/// [`Global`](MemoryTier::Global) when any tag is in the global set, otherwise
/// [`Agent`](MemoryTier::Agent).
///
/// # Examples
///
/// ```ignore
/// # use super::{
///     MemoryTier, RuleBasedTierClassifier, TierClassifier,
/// };
/// use claw_memory::MemoryDraft;
///
/// let classifier = RuleBasedTierClassifier::default();
///
/// // A user-level fact (no hint) routes to the global store.
/// let preference = MemoryDraft::new("Uses Home Assistant").with_tags(["preference".into()]);
/// assert_eq!(classifier.classify(&preference), MemoryTier::Global);
///
/// // A task note routes to the agent store.
/// let note = MemoryDraft::new("Deploy step needs sudo").with_tags(["task".into()]);
/// assert_eq!(classifier.classify(&note), MemoryTier::Agent);
/// ```
#[derive(Clone, Copy, Debug, Default)]
pub struct RuleBasedTierClassifier;

impl RuleBasedTierClassifier {
    /// A ready-to-share classifier with the default global tags.
    pub fn shared() -> Arc<dyn TierClassifier> {
        Arc::new(Self)
    }
}

impl TierClassifier for RuleBasedTierClassifier {
    fn classify(&self, draft: &MemoryDraft) -> MemoryTier {
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
}
