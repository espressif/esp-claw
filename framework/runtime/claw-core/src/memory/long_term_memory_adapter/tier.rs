//! Memory tiering: routing a fact to the **global** (shared across all agents)
//! or **agent** (private to one agent) long-term store.
//!
//! Tiering is a policy the agent must never expose to the model: the memory
//! tools take no `scope`/`tier` parameter, so the LLM cannot be misdirected into
//! choosing a store. Instead every new fact passes through a [`TierClassifier`]
//! that decides deterministically — honoring an optional upstream `hint` (e.g.
//! from an extractor) and otherwise falling back to a rule over the fact's tags.

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
/// Kept as an injectable seam so the routing policy can be swapped (a smarter
/// classifier, a test double) without touching the adapter. A `hint` lets an
/// upstream producer (an extractor that already reasoned about scope) override
/// the rule; `None` means "decide for me".
pub trait TierClassifier: Send + Sync {
    /// Decide the tier for `draft`, preferring `hint` when present.
    fn classify(&self, draft: &MemoryDraft, hint: Option<MemoryTier>) -> MemoryTier;
}

/// Tags that, by default, mark a fact as globally shared (user-level, not
/// task-level): identity and standing preferences belong to every agent.
const DEFAULT_GLOBAL_TAGS: &[&str] = &["profile", "preference", "identity", "user"];

/// The default [`TierClassifier`]: honor a `hint`, else route to
/// [`Global`](MemoryTier::Global) when any tag is in the global set, otherwise
/// [`Agent`](MemoryTier::Agent).
///
/// # Examples
///
/// ```
/// use claw_core::memory::{MemoryTier, RuleBasedTierClassifier, TierClassifier};
/// use claw_memory::MemoryDraft;
///
/// let classifier = RuleBasedTierClassifier::default();
///
/// // A profile fact (no hint) routes to the global store.
/// let profile = MemoryDraft::new("Name is Ada").with_tags(["profile".into()]);
/// assert_eq!(classifier.classify(&profile, None), MemoryTier::Global);
///
/// // A task note routes to the agent store.
/// let note = MemoryDraft::new("Deploy step needs sudo").with_tags(["task".into()]);
/// assert_eq!(classifier.classify(&note, None), MemoryTier::Agent);
///
/// // An explicit hint always wins.
/// assert_eq!(classifier.classify(&note, Some(MemoryTier::Global)), MemoryTier::Global);
/// ```
#[derive(Clone, Debug)]
pub struct RuleBasedTierClassifier {
    global_tags: Vec<String>,
}

impl RuleBasedTierClassifier {
    /// Classifier with a custom set of global-marking tags.
    pub fn new(global_tags: impl IntoIterator<Item = String>) -> Self {
        Self {
            global_tags: global_tags.into_iter().collect(),
        }
    }

    /// A ready-to-share classifier with the default global tags.
    pub fn shared() -> Arc<dyn TierClassifier> {
        Arc::new(Self::default())
    }
}

impl Default for RuleBasedTierClassifier {
    fn default() -> Self {
        Self {
            global_tags: DEFAULT_GLOBAL_TAGS
                .iter()
                .map(|tag| tag.to_string())
                .collect(),
        }
    }
}

impl TierClassifier for RuleBasedTierClassifier {
    fn classify(&self, draft: &MemoryDraft, hint: Option<MemoryTier>) -> MemoryTier {
        if let Some(hint) = hint {
            return hint;
        }
        let is_global = draft.tags.iter().any(|tag| {
            self.global_tags
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
