//! Long-term-memory extraction, context projection, and model-callable tools.
//!
//! Global and per-agent stores appear as one catalog; id prefixes route writes
//! back to the owning store.

use std::sync::Arc;

use claw_interface::ClawFs;

mod adapter;
mod catalog;
mod extraction;
mod extraction_flow;
mod llm_extractor;
mod stores;
mod tier;
mod tools;

pub(crate) use extraction::Extractor;
use extraction::{ExtractionInput, MemoryOp, MemorySnapshot};
pub(crate) use llm_extractor::LlmExtractor;
pub(crate) use stores::{agent_store, global_store};
use tier::MemoryTier;

use self::catalog::CatalogCache;
use self::stores::MemoryStores;

/// A [`ContextAdapter`] over a dual-tier long-term store. See the module docs.
pub(crate) struct LongTermMemoryContextAdapter<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
    extractor: Arc<dyn Extractor>,
    /// Rebuilt only when a store version advances.
    catalog: CatalogCache,
    /// Highest transcript version already handed to extraction.
    extract_cursor: u64,
}
