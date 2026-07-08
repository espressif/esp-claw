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

use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{
    LongTermConfig, LongTermError, LongTermInitError, LongTermMemory, MemoryDraft, MemoryId,
    MemoryItem, MemoryPatch, StoreOutcome,
};
use claw_tool::Tool;
use serde_json::Value;

use crate::memory::traits::{ContextAdapter, ContextAdapterFuture, ContextAdapterInput, History};

mod async_llm;
mod extraction;
mod llm_compactor;
mod llm_extractor;
mod tier;
mod tools;

#[cfg(test)]
use extraction::ExtractedItem;
pub use extraction::{ExtractionInput, Extractor, MemoryOp, MemorySnapshot};
pub use llm_compactor::LlmCompactor;
pub use llm_extractor::LlmExtractor;
pub use tier::{MemoryTier, MemoryTierHint, RuleBasedTierClassifier, TierClassifier};

use self::tools::memory_tools;

/// Id prefix for the shared global store.
pub const GLOBAL_ID_PREFIX: &str = "g-";
/// Id prefix for the per-agent store.
pub const AGENT_ID_PREFIX: &str = "a-";

/// Default memory id used in logs.
const DEFAULT_MEMORY_ID: &str = "long_term";

/// Extraction throttle: after the first extraction, the transcript
/// [`version`](History::version) must advance by at least this much before the
/// next extraction runs.
///
/// Extraction is an LLM round-trip triggered from `contribute`, which runs on
/// every iteration (including each tool-loop step within one turn). Without a
/// gate a long tool loop would re-extract several times per turn. This bounds
/// that to at most once per `EXTRACT_MIN_VERSION_DELTA` transcript changes.
///
/// It is a *coarse* cost bound, not an exact per-turn count (one turn bumps the
/// version by a few appends). Skipping never loses a fact: the transcript is
/// durable, so the accumulated tail is picked up by a later turn — or, since the
/// cursor resets on restart, re-extracted whole on the next boot — and store
/// dedup absorbs the overlap. The very first extraction is never throttled, so a
/// short conversation still records its facts promptly.
const EXTRACT_MIN_VERSION_DELTA: u64 = 8;

/// Build a global long-term store under `dir` (minting `g-` ids).
///
/// # Errors
///
/// Propagates [`LongTermInitError`] when the journal exists but is unreadable.
pub fn global_store<F: ClawFs + 'static>(
    dir: &str,
) -> Result<LongTermMemory<F>, LongTermInitError> {
    LongTermMemory::new(LongTermConfig::new(dir, GLOBAL_ID_PREFIX))
}

/// Build a per-agent long-term store under `dir` (minting `a-` ids).
///
/// # Errors
///
/// Propagates [`LongTermInitError`] when the journal exists but is unreadable.
pub fn agent_store<F: ClawFs + 'static>(dir: &str) -> Result<LongTermMemory<F>, LongTermInitError> {
    LongTermMemory::new(LongTermConfig::new(dir, AGENT_ID_PREFIX))
}

/// The two stores plus the routing policy, shared (by cheap clone) between the
/// adapter and every memory tool handler.
///
/// Every clone refers to the same underlying stores (each [`LongTermMemory`] is
/// `Arc`-backed), so tool calls and extraction write the same data.
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
    /// extractor, or [`MemoryTierHint::Auto`] for a manual store).
    pub(crate) fn store(&self, draft: MemoryDraft, hint: MemoryTierHint) -> StoreOutcome {
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

    /// A compact `id`/`content`/`tags` view of every stored fact, for handing to
    /// the [`Extractor`] so it can cite an id when proposing an edit/removal.
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
    /// Build an adapter over the two stores, a tier `classifier`, and an
    /// `extractor`.
    pub fn new(
        agent: LongTermMemory<F>,
        global: LongTermMemory<F>,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Self {
        Self {
            id: DEFAULT_MEMORY_ID.to_string(),
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

    /// Run extraction when the transcript has advanced.
    ///
    /// Pull, not push: called from [`contribute`](ContextAdapter::contribute) on
    /// the tick thread, it self-detects new conversation via [`History::version`]
    /// against [`extract_cursor`](Self::extract_cursor). Dedup in the store
    /// absorbs facts re-extracted across turns.
    async fn maybe_schedule_extraction(&mut self, history: &dyn History) {
        let version = history.version();
        if version == self.extract_cursor {
            return; // transcript unchanged since the last extraction
        }
        // Throttle re-extraction: after the first pass, require the transcript to
        // have advanced by a minimum before spending another LLM round-trip. The
        // first extraction (`extract_cursor == 0`) always runs so short
        // conversations still record their facts.
        if self.extract_cursor != 0
            && version.saturating_sub(self.extract_cursor) < EXTRACT_MIN_VERSION_DELTA
        {
            return;
        }

        let snapshot = history.messages();
        let transcript = flatten_transcript(&snapshot);
        if transcript.trim().is_empty() {
            // Nothing to extract yet; leave the cursor so the first real content
            // still counts as the (unthrottled) first extraction.
            return;
        }
        self.extract_cursor = version;
        // Hand the extractor the current memory so it can propose edits/removals
        // (by id), not only additions.
        let existing = self.stores.snapshot();
        let input = ExtractionInput {
            transcript: &transcript,
            existing: &existing,
        };
        match self.extractor.extract(input).await {
            Ok(ops) => {
                for op in ops {
                    self.apply_op(op);
                }
            }
            Err(_) => {}
        }
    }

    /// Apply one extractor-proposed [`MemoryOp`] to the stores. Best-effort: an
    /// edit/removal naming an id the store no longer holds is skipped, not fatal
    /// (the model may cite a fact a concurrent tool call already changed).
    fn apply_op(&self, op: MemoryOp) {
        match op {
            MemoryOp::Add(item) => {
                let draft = MemoryDraft::new(item.content)
                    .with_tags(item.tags)
                    .with_keywords(item.keywords)
                    .with_source("extracted");
                self.stores.store(draft, item.tier);
            }
            MemoryOp::Replace { id, item } => {
                let patch = MemoryPatch {
                    content: Some(item.content),
                    tags: Some(item.tags),
                    keywords: Some(item.keywords),
                };
                let _ = self.stores.update(&id, patch);
            }
            MemoryOp::Forget { id } => {
                let _ = self.stores.forget(&id);
            }
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
    fn id(&self) -> &str {
        &self.id
    }

    fn prepare<'a>(&'a mut self, input: ContextAdapterInput<'a>) -> ContextAdapterFuture<'a> {
        Box::pin(async move {
            // Pull, not push: reading the transcript here is where this adapter
            // decides whether new conversation warrants extraction.
            self.maybe_schedule_extraction(input.history).await;
            self.refresh_catalog();
        })
    }

    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
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

    fn tools(&self) -> Vec<Tool> {
        memory_tools(self.stores.clone())
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

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::extraction::ExtractFuture;
    use super::*;
    use crate::memory::History;
    use claw_interface::MemFs;
    use claw_memory::MemoryId;
    use futures_lite::future::block_on;
    use serde_json::json;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Arc;

    /// A fixed-version transcript view for driving `maybe_schedule_extraction`.
    struct FakeHistory {
        version: u64,
    }
    impl History for FakeHistory {
        fn messages(&self) -> Arc<Value> {
            Arc::new(json!([{ "role": "user", "content": "remember this" }]))
        }
        fn version(&self) -> u64 {
            self.version
        }
    }

    /// An [`Extractor`] that counts calls and extracts nothing.
    struct CountingExtractor {
        calls: Arc<AtomicUsize>,
    }
    impl Extractor for CountingExtractor {
        fn extract<'a>(&'a self, _input: ExtractionInput<'a>) -> ExtractFuture<'a> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            Box::pin(async { Ok(Vec::new()) })
        }
    }

    struct NoopExtractor;

    impl Extractor for NoopExtractor {
        fn extract<'a>(&'a self, _input: ExtractionInput<'a>) -> ExtractFuture<'a> {
            Box::pin(async { Ok(Vec::new()) })
        }
    }

    fn adapter() -> LongTermMemoryContextAdapter<MemFs> {
        MemFs::default();
        LongTermMemoryContextAdapter::new(
            agent_store::<MemFs>("/m/agent").expect("agent store"),
            global_store::<MemFs>("/m/global").expect("global store"),
            RuleBasedTierClassifier::shared(),
            Arc::new(NoopExtractor),
        )
    }

    fn fact(content: &str) -> ExtractedItem {
        ExtractedItem {
            content: content.to_string(),
            tags: vec!["fact".to_string()],
            keywords: Vec::new(),
            tier: MemoryTierHint::Auto,
        }
    }

    #[test]
    fn apply_add_replace_forget_round_trip() {
        let adapter = adapter();

        adapter.apply_op(MemoryOp::Add(fact("Lives in Berlin")));
        let items = adapter.stores.list();
        assert_eq!(items.len(), 1);
        let id = items[0].id.clone();
        assert_eq!(items[0].content, "Lives in Berlin");

        // Replace edits the cited fact in place.
        adapter.apply_op(MemoryOp::Replace {
            id: id.clone(),
            item: fact("Lives in Munich"),
        });
        let items = adapter.stores.list();
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].content, "Lives in Munich");

        // Forget removes it.
        adapter.apply_op(MemoryOp::Forget { id });
        assert!(adapter.stores.list().is_empty());
    }

    #[test]
    fn apply_edit_on_unknown_id_is_a_noop() {
        let adapter = adapter();
        adapter.apply_op(MemoryOp::Add(fact("Has a dog")));

        // An id the store never held is skipped — never a panic, and
        // the live set is untouched.
        adapter.apply_op(MemoryOp::Forget {
            id: MemoryId::from("g-999"),
        });
        adapter.apply_op(MemoryOp::Replace {
            id: MemoryId::from("a-999"),
            item: fact("ghost edit"),
        });
        assert_eq!(adapter.stores.list().len(), 1);
    }

    #[test]
    fn extraction_is_throttled_after_the_first_pass() {
        let calls = Arc::new(AtomicUsize::new(0));
        MemFs::default();
        let mut adapter = LongTermMemoryContextAdapter::new(
            agent_store::<MemFs>("/m/agent").expect("agent store"),
            global_store::<MemFs>("/m/global").expect("global store"),
            RuleBasedTierClassifier::shared(),
            Arc::new(CountingExtractor {
                calls: Arc::clone(&calls),
            }),
        );

        // The first non-empty transcript always extracts.
        block_on(adapter.maybe_schedule_extraction(&FakeHistory { version: 3 }));
        assert_eq!(calls.load(Ordering::SeqCst), 1);

        // A small advance is below the delta and is throttled away.
        block_on(adapter.maybe_schedule_extraction(&FakeHistory { version: 5 }));
        assert_eq!(calls.load(Ordering::SeqCst), 1);

        // Crossing the delta since the last extraction runs it again.
        block_on(adapter.maybe_schedule_extraction(&FakeHistory {
            version: 3 + EXTRACT_MIN_VERSION_DELTA,
        }));
        assert_eq!(calls.load(Ordering::SeqCst), 2);
    }
}
