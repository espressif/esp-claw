use std::sync::Arc;

use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::LongTermMemory;
use claw_tool::Tool;

use crate::memory::traits::{ContextAdapter, ContextAdapterFuture, ContextAdapterInput};

use super::catalog::{render_catalog, CatalogCache};
use super::stores::MemoryStores;
use super::tools::memory_tools;
use super::{Extractor, LongTermMemoryContextAdapter, TierClassifier};

impl<F: ClawFs + 'static> LongTermMemoryContextAdapter<F> {
    /// Build an adapter over the two stores, a tier `classifier`, and an
    /// `extractor`.
    pub fn new(
        agent: LongTermMemory<F>,
        global: LongTermMemory<F>,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Self {
        Self {
            stores: MemoryStores {
                global,
                agent,
                classifier,
            },
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
    fn prepare<'a>(&'a mut self, input: ContextAdapterInput<'a>) -> ContextAdapterFuture<'a> {
        Box::pin(async move {
            // Pull, not push: reading the transcript here is where this adapter
            // decides whether new conversation warrants extraction.
            self.maybe_schedule_extraction(input.history).await;
            self.refresh_catalog();
        })
    }

    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
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

    fn tools(&self) -> Vec<Tool> {
        memory_tools(self.stores.clone())
    }
}
