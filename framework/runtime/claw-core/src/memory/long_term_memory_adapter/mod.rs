//! Long-term-memory extraction, context projection, and model-callable tools.
//!
//! Global and per-agent stores appear as one catalog; id prefixes route writes
//! back to the owning store.

use std::sync::Arc;

use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::LongTermMemory;
use claw_tool::ToolGroup;

use crate::memory::traits::{ContextAdapter, ContextAdapterFuture, History};

mod extraction;
mod extraction_flow;
mod llm_extractor;
mod stores;
mod tier;
mod tools;

use self::stores::MemoryStores;
use self::tools::memory_tools;
pub(crate) use extraction::Extractor;
use extraction::{ExtractionInput, MemoryOp, MemorySnapshot};
pub(crate) use llm_extractor::LlmExtractor;
pub(crate) use stores::{agent_store, global_store};
use tier::MemoryTier;

/// The adapter's rendered-catalog cache, keyed on each store's change version.
#[derive(Default)]
struct CatalogCache {
    global_version: u64,
    agent_version: u64,
    global_block: String,
    agent_block: String,
    /// `false` until the first render populates the blocks (version 0 is a real
    /// state, an empty store, so a flag distinguishes "never rendered").
    primed: bool,
}

/// A [`ContextAdapter`] over a dual-tier long-term store. See the module docs.
pub(crate) struct LongTermMemoryContextAdapter<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
    extractor: Arc<dyn Extractor>,
    /// Rebuilt only when a store version advances.
    catalog: CatalogCache,
    /// Highest transcript version already handed to extraction.
    extract_cursor: u64,
}

impl<F: ClawFs + 'static> LongTermMemoryContextAdapter<F> {
    /// Build an adapter over the two stores and an `extractor`.
    pub(crate) fn new(
        agent: LongTermMemory<F>,
        global: LongTermMemory<F>,
        extractor: Arc<dyn Extractor>,
    ) -> Self {
        Self {
            stores: MemoryStores { global, agent },
            extractor,
            catalog: CatalogCache::default(),
            extract_cursor: 0,
        }
    }

    fn refresh_catalog(&mut self) {
        let global_version = self.stores.global.version();
        let agent_version = self.stores.agent.version();
        // Rebuild a block's text only when its store changed (or on first refresh).
        if !self.catalog.primed || self.catalog.global_version != global_version {
            self.catalog.global_block = render_catalog(
                "Shared long-term memory topics",
                &self.stores.global.catalog(),
            );
            self.catalog.global_version = global_version;
        }
        if !self.catalog.primed || self.catalog.agent_version != agent_version {
            self.catalog.agent_block =
                render_catalog("Your long-term memory topics", &self.stores.agent.catalog());
            self.catalog.agent_version = agent_version;
        }
        self.catalog.primed = true;
    }
}

impl<F: ClawFs + 'static> ContextAdapter for LongTermMemoryContextAdapter<F> {
    fn prepare<'a>(&'a mut self, history: &'a dyn History) -> ContextAdapterFuture<'a> {
        Box::pin(async move {
            // Pull, not push: reading the transcript here is where this adapter
            // decides whether new conversation warrants extraction.
            self.maybe_schedule_extraction(history).await;
            self.refresh_catalog();
        })
    }

    fn contribute(&mut self, output: &mut ContextSink<'_>) {
        // Borrow the cached strings into the blocks; `Context::with` copies them
        // only on a real change, so an unchanged catalog allocates nothing here.
        output.block(Block::new(
            BlockKind::GlobalMemory,
            self.catalog.global_block.as_str(),
        ));
        output.block(Block::new(
            BlockKind::AgentMemory,
            self.catalog.agent_block.as_str(),
        ));
    }

    fn tools(&self) -> Option<ToolGroup> {
        Some(memory_tools(self.stores.clone()))
    }
}

/// Render a label catalog as a single durable-context line, or empty when there
/// are no labels (the context then drops the block).
fn render_catalog(header: &str, labels: &[String]) -> String {
    if labels.is_empty() {
        String::new()
    } else {
        format!("{header}: {}", labels.join(", "))
    }
}
