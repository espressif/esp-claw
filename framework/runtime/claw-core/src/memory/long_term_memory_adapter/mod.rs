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

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{
    LongTermConfig, LongTermError, LongTermMemory, MemoryDraft, MemoryId, MemoryItem, MemoryPatch,
    StoreOutcome,
};
use claw_tool::ToolGroup;
use claw_utils::SharedTaskPool;
use serde_json::Value;

use crate::memory::traits::{ContextAdapter, ContextAdapterInput, History};

mod extraction;
mod llm_compactor;
mod llm_extractor;
mod tier;
mod tools;

pub use extraction::{ExtractError, ExtractedItem, Extractor, NoopExtractor};
pub use llm_compactor::LlmCompactor;
pub use llm_extractor::LlmExtractor;
pub use tier::{MemoryTier, RuleBasedTierClassifier, TierClassifier};

use self::tools::memory_tool_group;

/// Id prefix for the shared global store.
pub const GLOBAL_ID_PREFIX: &str = "g-";
/// Id prefix for the per-agent store.
pub const AGENT_ID_PREFIX: &str = "a-";

/// Default memory id used in logs.
const DEFAULT_MEMORY_ID: &str = "long_term";

/// Build a global long-term store under `dir` (minting `g-` ids).
pub fn global_store<F: ClawFs + 'static>(dir: impl Into<String>, fs: F) -> LongTermMemory<F> {
    LongTermMemory::new(LongTermConfig::new(dir, GLOBAL_ID_PREFIX), fs)
}

/// Build a per-agent long-term store under `dir` (minting `a-` ids).
pub fn agent_store<F: ClawFs + 'static>(dir: impl Into<String>, fs: F) -> LongTermMemory<F> {
    LongTermMemory::new(LongTermConfig::new(dir, AGENT_ID_PREFIX), fs)
}

/// The two stores plus the routing policy, shared (by cheap clone) between the
/// adapter, the extraction job, and every memory tool handler.
///
/// Every clone refers to the same underlying stores (each [`LongTermMemory`] is
/// `Arc`-backed), so a tool call on the tick thread and an extraction job on a
/// pool worker write the same data.
pub(crate) struct MemoryStores<F: ClawFs + 'static> {
    global: LongTermMemory<F>,
    agent: LongTermMemory<F>,
    classifier: Arc<dyn TierClassifier>,
}

impl<F: ClawFs + 'static> Clone for MemoryStores<F> {
    fn clone(&self) -> Self {
        Self {
            global: self.global.clone(),
            agent: self.agent.clone(),
            classifier: Arc::clone(&self.classifier),
        }
    }
}

impl<F: ClawFs + 'static> MemoryStores<F> {
    /// Store a draft, routing it to a tier via the classifier (`hint` from an
    /// extractor, or `None` for a manual store).
    pub(crate) fn store(&self, draft: MemoryDraft, hint: Option<MemoryTier>) -> StoreOutcome {
        match self.classifier.classify(&draft, hint) {
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

    /// The store that owns `id` (by its prefix; defaults to the agent store for
    /// an unrecognized prefix, which then reports `NotFound`).
    fn store_for(&self, id: &MemoryId) -> &LongTermMemory<F> {
        if id.as_str().starts_with(GLOBAL_ID_PREFIX) {
            &self.global
        } else {
            &self.agent
        }
    }
}

/// A [`ContextAdapter`] over a dual-tier long-term store. See the module docs.
pub struct LongTermMemoryContextAdapter<F: ClawFs + 'static> {
    id: String,
    stores: MemoryStores<F>,
    extractor: Arc<dyn Extractor>,
    pool: Arc<SharedTaskPool>,
    /// Cached rendered catalog blocks, rebuilt by [`refresh`](Self::refresh) only
    /// when a store's version advances — so an unchanged store re-lends its blocks
    /// with no work. Plain fields: the agent calls `refresh` under `&mut self`, so
    /// no interior lock is needed.
    catalog: CatalogCache,
    /// Highest transcript [`History::version`] already handed to an extraction
    /// job. Refresh schedules a fresh extraction only when the transcript has
    /// advanced past this, so an unchanged conversation costs nothing.
    extract_cursor: u64,
    /// Single-flight guard: at most one extraction job in the pool at a time.
    /// New transcript content that lands while one runs simply re-triggers on a
    /// later refresh, coalescing a busy multi-round turn into roughly one job.
    /// `Arc` so the pool job can clear it on completion off the tick thread.
    extraction_in_flight: Arc<AtomicBool>,
}

/// The adapter's rendered-catalog cache, keyed on each store's change version.
#[derive(Default)]
struct CatalogCache {
    global_version: u64,
    agent_version: u64,
    global_block: String,
    agent_block: String,
    /// `false` until the first render populates the blocks (version 0 is a real
    /// state — an empty store — so a flag distinguishes "never rendered").
    primed: bool,
}

impl<F: ClawFs + 'static> LongTermMemoryContextAdapter<F> {
    /// Build an adapter over the two stores, a tier `classifier`, an `extractor`,
    /// and the shared memory `pool` that runs extraction off the tick path.
    pub fn new(
        agent: LongTermMemory<F>,
        global: LongTermMemory<F>,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
        pool: Arc<SharedTaskPool>,
    ) -> Self {
        Self {
            id: DEFAULT_MEMORY_ID.to_string(),
            stores: MemoryStores {
                global,
                agent,
                classifier,
            },
            extractor,
            pool,
            catalog: CatalogCache::default(),
            extract_cursor: 0,
            extraction_in_flight: Arc::new(AtomicBool::new(false)),
        }
    }

    /// Schedule a background extraction when the transcript has advanced.
    ///
    /// Pull, not push: called from [`contribute`](ContextAdapter::contribute) on
    /// the tick thread, it self-detects new conversation via [`History::version`]
    /// against [`extract_cursor`](Self::extract_cursor). It snapshots only when
    /// it actually schedules (a cheap `Arc` clone of the canonical transcript),
    /// and the single-flight guard coalesces a busy multi-round turn into roughly
    /// one job. Dedup in the store absorbs facts re-extracted across turns.
    fn maybe_schedule_extraction(&mut self, history: &dyn History) {
        let version = history.version();
        if version == self.extract_cursor {
            return; // transcript unchanged since the last extraction
        }
        if self.extraction_in_flight.swap(true, Ordering::AcqRel) {
            return; // one already running; a later refresh re-triggers
        }
        self.extract_cursor = version;

        let snapshot = history.messages();
        let extractor = Arc::clone(&self.extractor);
        let stores = self.stores.clone();
        let memory_id = self.id.clone();
        let in_flight = Arc::clone(&self.extraction_in_flight);
        self.pool.submit(Box::new(move || {
            let transcript = flatten_transcript(&snapshot);
            if !transcript.trim().is_empty() {
                match extractor.extract(&transcript) {
                    Ok(items) => {
                        for item in items {
                            let draft = MemoryDraft::new(item.content)
                                .with_tags(item.tags)
                                .with_keywords(item.keywords)
                                .with_source("extracted");
                            stores.store(draft, item.tier);
                        }
                    }
                    Err(error) => {
                        tracing::warn!(%error, memory = %memory_id, "memory extraction failed")
                    }
                }
            }
            in_flight.store(false, Ordering::Release);
        }));
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
    fn id(&self) -> &str {
        &self.id
    }

    fn contribute(&mut self, input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        // Pull, not push: reading the transcript here is also where this adapter
        // decides whether new conversation warrants a background extraction.
        self.maybe_schedule_extraction(input.history);
        self.refresh_catalog();
        // Borrow the cached strings into the blocks — `Context::with` copies them
        // only on a real change, so an unchanged catalog allocates nothing here.
        // An empty catalog renders to an empty block, which clears that section.
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
        Some(memory_tool_group(self.stores.clone()))
    }
}

/// Flatten a transcript snapshot (a JSON array of chat messages) into the
/// role-prefixed plain text an [`Extractor`] reads. Messages without string
/// content (e.g. an assistant turn carrying only `tool_calls`) are skipped.
fn flatten_transcript(messages: &Value) -> String {
    let Some(items) = messages.as_array() else {
        return String::new();
    };
    let mut out = String::new();
    for message in items {
        let role = message
            .get("role")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let content = message
            .get("content")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if role.is_empty() || content.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push('\n');
        }
        out.push_str(role);
        out.push_str(": ");
        out.push_str(content);
    }
    out
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
