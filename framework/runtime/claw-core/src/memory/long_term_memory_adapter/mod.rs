//! [`LongTermMemoryContextAdapter`] — the [`ContextAdapter`] that fronts the
//! dual-tier long-term store, owning extraction scheduling, context contribution,
//! and the memory tools.
//!
//! It holds two [`LongTermMemory`] stores — a **global** one shared across every
//! agent and an **agent** one private to this agent — and presents them to the
//! model as a *single* memory: the tools take no tier parameter, recall/list
//! merge both stores, and stores route through the [`TierClassifier`]. The model
//! therefore never has to reason about where a fact lives; it just remembers and
//! recalls.
//!
//! The id prefix a store mints (`g-` global, `a-` agent) is the routing key for
//! id-addressed operations (`update`/`forget`): the prefix is opaque to the
//! model but lets the adapter send an edit back to the store that owns the item.

use std::sync::Arc;

use claw_interface::ClawFs;

mod adapter;
mod async_llm;
mod catalog;
mod extraction;
mod extraction_flow;
mod llm_compactor;
mod llm_extractor;
mod stores;
mod tier;
mod tools;

pub use extraction::{ExtractionInput, Extractor, MemoryOp, MemorySnapshot};
pub use llm_compactor::LlmCompactor;
pub use llm_extractor::LlmExtractor;
pub use stores::{agent_store, global_store};
pub use tier::{MemoryTier, RuleBasedTierClassifier, TierClassifier};

use self::catalog::CatalogCache;
use self::stores::MemoryStores;

/// A [`ContextAdapter`] over a dual-tier long-term store. See the module docs.
pub struct LongTermMemoryContextAdapter<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
    extractor: Arc<dyn Extractor>,
    /// Cached rendered catalog blocks, rebuilt by [`refresh`](Self::refresh) only
    /// when a store's version advances — so an unchanged store re-lends its blocks
    /// with no work. Plain fields: the agent calls `refresh` under `&mut self`, so
    /// no interior lock is needed.
    catalog: CatalogCache,
    /// Highest transcript [`History::version`] already handed to extraction.
    /// Refresh extracts only when the transcript has advanced past this, so an
    /// unchanged conversation costs nothing.
    extract_cursor: u64,
}
