use std::sync::Arc;

use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::LongTermMemory;
use claw_tool::ToolGroup;

use crate::memory::traits::{ContextAdapter, ContextAdapterFuture, History};

use super::catalog::{render_catalog, CatalogCache};
use super::stores::MemoryStores;
use super::tools::memory_tools;
use super::{Extractor, LongTermMemoryContextAdapter};

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

    pub(super) fn refresh_catalog(&mut self) {
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
