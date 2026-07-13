use claw_interface::ClawFs;
use claw_memory::{
    LongTermError, LongTermInitError, LongTermMemory, MemoryDraft, MemoryId, MemoryItem,
    MemoryPatch, StoreOutcome,
};

use super::tier::classify_tier;
use super::{MemorySnapshot, MemoryTier};

/// Id prefix for the shared global store.
pub(super) const GLOBAL_ID_PREFIX: &str = "g-";
/// Id prefix for the per-agent store.
pub(super) const AGENT_ID_PREFIX: &str = "a-";

/// Build a global long-term store under `dir` (minting `g-` ids).
///
/// # Errors
///
/// Propagates [`LongTermInitError`] when the journal exists but is unreadable.
pub(crate) fn global_store<F: ClawFs + 'static>(
    dir: &str,
) -> Result<LongTermMemory<F>, LongTermInitError> {
    LongTermMemory::new(dir, GLOBAL_ID_PREFIX)
}

/// Build a per-agent long-term store under `dir` (minting `a-` ids).
///
/// # Errors
///
/// Propagates [`LongTermInitError`] when the journal exists but is unreadable.
pub(crate) fn agent_store<F: ClawFs + 'static>(
    dir: &str,
) -> Result<LongTermMemory<F>, LongTermInitError> {
    LongTermMemory::new(dir, AGENT_ID_PREFIX)
}

/// The two stores, shared (by cheap clone) between the adapter and every memory
/// tool handler.
pub(crate) struct MemoryStores<F: ClawFs + 'static> {
    pub(super) global: LongTermMemory<F>,
    pub(super) agent: LongTermMemory<F>,
}

impl<F: ClawFs + 'static> Clone for MemoryStores<F> {
    fn clone(&self) -> Self {
        Self {
            global: self.global.clone(),
            agent: self.agent.clone(),
        }
    }
}

impl<F: ClawFs + 'static> MemoryStores<F> {
    /// Store a draft in the tier determined by its tags.
    pub(crate) fn store(&self, draft: MemoryDraft) -> StoreOutcome {
        match classify_tier(&draft) {
            MemoryTier::Global => self.global.store(draft),
            MemoryTier::Agent => self.agent.store(draft),
        }
    }

    /// Recall across both stores (global first), capped at `limit` total.
    pub(crate) fn recall(
        &self,
        labels: &[String],
        query: Option<&str>,
        limit: usize,
    ) -> Vec<MemoryItem> {
        let mut hits = self.global.recall(labels, query, limit);
        hits.extend(self.agent.recall(labels, query, limit));
        hits.truncate(limit);
        hits
    }

    /// All facts across both stores (global first).
    pub(crate) fn list(&self) -> Vec<MemoryItem> {
        let mut items = self.global.list();
        items.extend(self.agent.list());
        items
    }

    pub(crate) fn snapshot(&self) -> Vec<MemorySnapshot> {
        self.list()
            .into_iter()
            .map(|item| MemorySnapshot {
                id: item.id,
                content: item.content,
                tags: item.tags,
            })
            .collect()
    }

    /// Apply a patch to the item with `id`, routing by its prefix.
    pub(crate) fn update(
        &self,
        id: &MemoryId,
        patch: MemoryPatch,
    ) -> Result<MemoryItem, LongTermError> {
        self.store_for(id).update(id, patch)
    }

    /// Forget the item with `id`, routing by its prefix.
    pub(crate) fn forget(&self, id: &MemoryId) -> Result<(), LongTermError> {
        self.store_for(id).forget(id)
    }

    fn store_for(&self, id: &MemoryId) -> &LongTermMemory<F> {
        if id.as_str().starts_with(GLOBAL_ID_PREFIX) {
            &self.global
        } else {
            &self.agent
        }
    }
}
