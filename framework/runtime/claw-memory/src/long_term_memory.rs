//! Long-term memory: a small, durable, label-indexed store of distilled facts.
//!
//! Where [`crate::transcript_store`] holds the *recent transcript* and ages it
//! out through compaction, long-term memory holds the handful of *distilled
//! facts* that should outlive any one transcript — the user's name, a standing
//! preference, an identity note. It is deliberately tiny (tens of items, not
//! thousands) and addressed by **labels** (free-form tags): the agent recalls
//! "everything tagged `preference`" rather than scanning a vector index.
//!
//! This type is **pure storage**. It does not know how facts are produced
//! (manual tool calls vs. LLM extraction), it does not know about memory tiers
//! (global vs. per-agent), and it never calls an LLM. Those policies live one
//! layer up in `claw_core`; here we only persist, dedup, retrieve, and reclaim.
//!
//! # Storage layout
//!
//! A single append-only journal, `memory_records.jsonl`, under the `dir` passed
//! to [`LongTermMemory::new`]. Each line is one [`Record`]:
//! - `Put` — a full snapshot of an item (a fresh store *or* an update). On
//!   replay, the last `Put` for an id wins, so an update just appends.
//! - `Del` — a tombstone removing an id.
//!
//! Load replays the journal into the live set in memory; a torn trailing line
//! (crash mid-append) fails to parse and is skipped. When superseded `Put`s and
//! tombstones (`dead` records) pile up past the internal compaction threshold,
//! the journal is rewritten atomically from the live set, dropping the dead
//! lines.
//!
//! # Concurrency
//!
//! All state sits behind one `Mutex` inside an `Arc`, so the agent context path
//! and memory tools can write the *same* store through cheap [`Clone`]s of the
//! handle. The store itself is thread-safe; higher layers decide whether a
//! particular extraction or tool path is local or worker-driven.

use std::marker::PhantomData;
use std::sync::{Arc, Mutex, MutexGuard};

use serde::{Deserialize, Serialize};

use claw_interface::{ClawFs, FsError};

/// Journal filename under the store directory.
const RECORDS_FILE: &str = "memory_records.jsonl";
/// Default number of dead journal lines tolerated before a rewrite.
const DEFAULT_COMPACT_DEAD_THRESHOLD: u32 = 32;

/// A stable identifier for one stored fact.
///
/// Minted by the store at first [`store`](LongTermMemory::store) as
/// `{id_prefix}{seq}` (e.g. `g-7`, `a-3`) and stable across updates. The prefix
/// is opaque to this crate — the `claw_core` adapter uses it to route an id back
/// to the tier (global vs. agent) that owns it.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct MemoryId(String);

impl MemoryId {
    /// The id as a string slice.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Display for MemoryId {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl From<&str> for MemoryId {
    fn from(value: &str) -> Self {
        MemoryId(value.to_string())
    }
}

/// One stored fact, as persisted and as returned from recall/list.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryItem {
    /// Stable identity (see [`MemoryId`]).
    pub id: MemoryId,
    /// The distilled fact, in the agent's own words.
    pub content: String,
    /// Free-form labels this fact is filed under; the retrieval key.
    pub tags: Vec<String>,
    /// Extra search terms that need not appear in `content`.
    pub keywords: Vec<String>,
    /// Where the fact came from (e.g. `"manual"`, `"extracted"`); opaque here.
    pub source: String,
    /// Monotonic insertion order, used to sort newest-first. Set by the store.
    pub seq: u64,
}

/// A new fact to store. The store assigns the [`MemoryId`] and `seq`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MemoryDraft {
    /// The distilled fact.
    pub content: String,
    /// Labels to file it under.
    pub tags: Vec<String>,
    /// Extra search terms.
    pub keywords: Vec<String>,
    /// Provenance marker (opaque to the store).
    pub source: String,
}

impl MemoryDraft {
    /// A draft with the given content and no tags/keywords/source.
    pub fn new(content: impl Into<String>) -> Self {
        Self {
            content: content.into(),
            ..Self::default()
        }
    }

    /// Set the labels (builder style).
    pub fn with_tags(mut self, tags: impl IntoIterator<Item = String>) -> Self {
        self.tags = tags.into_iter().collect();
        self
    }

    /// Set the keywords (builder style).
    pub fn with_keywords(mut self, keywords: impl IntoIterator<Item = String>) -> Self {
        self.keywords = keywords.into_iter().collect();
        self
    }

    /// Set the provenance marker (builder style).
    pub fn with_source(mut self, source: impl Into<String>) -> Self {
        self.source = source.into();
        self
    }
}

/// A partial edit to an existing fact: `Some` replaces the field, `None` leaves
/// it untouched.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MemoryPatch {
    /// Replacement content, if changing it.
    pub content: Option<String>,
    /// Replacement labels, if changing them.
    pub tags: Option<Vec<String>>,
    /// Replacement keywords, if changing them.
    pub keywords: Option<Vec<String>>,
}

/// The outcome of a [`store`](LongTermMemory::store).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StoreOutcome {
    /// A new item was created.
    Created(MemoryItem),
    /// An existing near-duplicate was found; nothing was written.
    Duplicate(MemoryItem),
}

impl StoreOutcome {
    /// The stored (or pre-existing duplicate) item, regardless of outcome.
    pub fn item(&self) -> &MemoryItem {
        match self {
            StoreOutcome::Created(item) | StoreOutcome::Duplicate(item) => item,
        }
    }
}

/// Failure from an operation that addresses an item by id.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum LongTermError {
    /// No live item has the given id.
    #[error("no memory item with id {0}")]
    NotFound(MemoryId),
}

/// Failure building a [`LongTermMemory`] from its on-disk journal.
///
/// A *missing* journal is not an error (the store starts empty); this is
/// returned only when a journal is present but cannot be read, so a genuine I/O
/// failure is never silently mistaken for an empty store.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum LongTermInitError {
    /// The journal exists but could not be read.
    #[error("long-term memory journal {path} is unreadable: {source}")]
    Unreadable {
        /// The journal path that failed to load.
        path: String,
        /// The underlying filesystem error.
        #[source]
        source: FsError,
    },
}

/// One line of the journal.
#[derive(Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
enum Record {
    /// A full item snapshot (store or update); last writer per id wins on replay.
    Put(MemoryItem),
    /// A tombstone removing an id.
    Del { id: MemoryId },
}

#[derive(Debug, thiserror::Error)]
enum LongTermPersistError {
    #[error("serialize record failed: {0}")]
    SerializeRecord(#[source] serde_json::Error),
    #[error("compaction serialize failed: {0}")]
    CompactSerialize(#[source] serde_json::Error),
}

/// The lock-protected contents of the store.
#[derive(Default)]
struct State {
    /// Live items, in insertion order (ascending `seq`).
    items: Vec<MemoryItem>,
    /// Next `seq`/id suffix to mint.
    next_seq: u64,
    /// Superseded `Put`s + tombstones since the last rewrite.
    dead: u32,
    /// Cached distinct-tag catalog; invalidated on any mutation.
    catalog_cache: Option<Vec<String>>,
    /// Monotonic change counter, bumped on every mutation that alters the live
    /// set. Lets a reader (e.g. a context renderer) cache derived output and
    /// rebuild only when this advances, without diffing the items.
    version: u64,
    /// The last journal write/append failure, if any, cleared on the next
    /// successful persist. Best-effort writes keep in-memory state authoritative,
    /// but a caller can observe a persistence failure via
    /// [`LongTermMemory::last_persist_error`] instead of only seeing a log line.
    last_persist_error: Option<FsError>,
}

struct Inner<F: ClawFs + 'static> {
    path: String,
    id_prefix: String,
    _fs: PhantomData<fn() -> F>,
    state: Mutex<State>,
}

/// A durable, label-indexed store of distilled facts. See the module docs for
/// the storage layout and concurrency model.
///
/// Cheap to [`Clone`] (clones the `Arc`, not the backend); every clone refers to
/// the same store, so context adapters and memory tools share one live view.
///
/// # Examples
///
/// ```
/// use claw_interface::MemFs;
/// use claw_memory::{LongTermMemory, MemoryDraft, StoreOutcome};
///
/// # MemFs::new();
/// let memory = LongTermMemory::<MemFs>::new("/m", "g-")
///     .expect("a fresh MemFs has no journal, so the store starts empty");
///
/// // Store a fact tagged `preference`, then recall by that label.
/// let stored = memory_store(
///     MemoryDraft::new("Prefers tea over coffee").with_tags(["preference".into()]),
/// );
/// assert!(matches!(stored, StoreOutcome::Created(_)));
///
/// let hits = memory_recall(&["preference".to_string()], None, 10);
/// assert_eq!(hits.len(), 1);
/// assert_eq!(hits[0].content, "Prefers tea over coffee");
///
/// // The catalog lists the distinct labels in use.
/// assert_eq!(memory.catalog(), vec!["preference".to_string()]);
/// ```
pub struct LongTermMemory<F: ClawFs + 'static> {
    inner: Arc<Inner<F>>,
}

// Manual `Clone`: only the `Arc` is cloned, so this is cheap and does not
// require `F: Clone`.
impl<F: ClawFs + 'static> Clone for LongTermMemory<F> {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl<F: ClawFs + 'static> LongTermMemory<F> {
    /// Build the store, restoring its journal if present. Best-effort creates
    /// `dir`.
    ///
    /// # Errors
    ///
    /// [`LongTermInitError::Unreadable`] when the journal exists but cannot be
    /// read — a genuine I/O failure is never silently mistaken for an empty
    /// store. A *missing* journal is not an error: the store starts empty.
    pub fn new(dir: &str, id_prefix: &str) -> Result<Self, LongTermInitError> {
        let path = journal_path(dir);
        if let Err(error) = F::create_dir_all(dir) {
            log::warn!("long-term memory {dir}: create dir failed: {error}");
        }
        let state = load_state::<F>(&path).map_err(|source| LongTermInitError::Unreadable {
            path: path.clone(),
            source,
        })?;
        Ok(Self {
            inner: Arc::new(Inner {
                path,
                id_prefix: id_prefix.to_string(),
                _fs: PhantomData,
                state: Mutex::new(state),
            }),
        })
    }

    /// The last journal persistence failure, if any.
    ///
    /// Journal appends are best-effort: a failure leaves the in-memory state
    /// authoritative and logs, but the write did not land. This exposes that
    /// failure so a caller can react (retry, surface a warning) instead of
    /// discovering the loss only on the next reboot. Cleared on the next
    /// successful persist.
    pub fn last_persist_error(&self) -> Option<FsError> {
        self.lock().last_persist_error.clone()
    }

    /// Store a new fact, or return the existing near-duplicate unchanged.
    ///
    /// Dedup is by normalized content (case- and whitespace-insensitive): storing
    /// a fact whose content matches a live item yields
    /// [`StoreOutcome::Duplicate`] and writes nothing.
    pub fn store(&self, draft: MemoryDraft) -> StoreOutcome {
        let mut state = self.lock();
        let key = normalize(&draft.content);
        if let Some(existing) = state
            .items
            .iter()
            .find(|item| normalize(&item.content) == key)
        {
            return StoreOutcome::Duplicate(existing.clone());
        }

        let seq = state.next_seq;
        state.next_seq = seq.saturating_add(1);
        let item = MemoryItem {
            id: MemoryId(format!("{}{}", self.inner.id_prefix, seq)),
            content: draft.content,
            tags: draft.tags,
            keywords: draft.keywords,
            source: draft.source,
            seq,
        };
        state.items.push(item.clone());
        state.catalog_cache = None;
        state.version = state.version.saturating_add(1);
        state.last_persist_error = self.append_record(&Record::Put(item.clone())).err();
        StoreOutcome::Created(item)
    }

    /// Recall live facts filed under any of `labels`, optionally narrowed to those
    /// whose content or keywords contain `query` (case-insensitive), newest first,
    /// capped at `limit`.
    ///
    /// Empty `labels` matches every live item (query/limit still apply).
    pub fn recall(&self, labels: &[String], query: Option<&str>, limit: usize) -> Vec<MemoryItem> {
        let state = self.lock();
        let query = query.map(str::to_lowercase);
        let mut hits: Vec<MemoryItem> = state
            .items
            .iter()
            .filter(|item| labels.is_empty() || item.tags.iter().any(|tag| labels.contains(tag)))
            .filter(|item| {
                query
                    .as_deref()
                    .is_none_or(|needle| matches_query(item, needle))
            })
            .cloned()
            .collect();
        hits.sort_by_key(|item| std::cmp::Reverse(item.seq));
        hits.truncate(limit);
        hits
    }

    /// All live facts, newest first.
    pub fn list(&self) -> Vec<MemoryItem> {
        let state = self.lock();
        let mut items = state.items.clone();
        items.sort_by_key(|item| std::cmp::Reverse(item.seq));
        items
    }

    /// Apply a partial edit to the item with `id`.
    ///
    /// # Errors
    ///
    /// [`LongTermError::NotFound`] if no live item has that id.
    pub fn update(&self, id: &MemoryId, patch: MemoryPatch) -> Result<MemoryItem, LongTermError> {
        let mut state = self.lock();
        let Some(item) = state.items.iter_mut().find(|item| &item.id == id) else {
            return Err(LongTermError::NotFound(id.clone()));
        };
        if let Some(content) = patch.content {
            item.content = content;
        }
        if let Some(tags) = patch.tags {
            item.tags = tags;
        }
        if let Some(keywords) = patch.keywords {
            item.keywords = keywords;
        }
        let updated = item.clone();
        state.catalog_cache = None;
        state.version = state.version.saturating_add(1);
        state.dead = state.dead.saturating_add(1);
        state.last_persist_error = self.append_record(&Record::Put(updated.clone())).err();
        self.maybe_compact(&mut state);
        Ok(updated)
    }

    /// Forget the item with `id`.
    ///
    /// # Errors
    ///
    /// [`LongTermError::NotFound`] if no live item has that id.
    pub fn forget(&self, id: &MemoryId) -> Result<(), LongTermError> {
        let mut state = self.lock();
        let Some(position) = state.items.iter().position(|item| &item.id == id) else {
            return Err(LongTermError::NotFound(id.clone()));
        };
        state.items.remove(position);
        state.catalog_cache = None;
        state.version = state.version.saturating_add(1);
        state.dead = state.dead.saturating_add(1);
        state.last_persist_error = self.append_record(&Record::Del { id: id.clone() }).err();
        self.maybe_compact(&mut state);
        Ok(())
    }

    /// The distinct labels currently in use, sorted. This is the retrieval
    /// "table of contents" the agent picks from. Cached between mutations.
    pub fn catalog(&self) -> Vec<String> {
        let mut state = self.lock();
        if let Some(cached) = &state.catalog_cache {
            return cached.clone();
        }
        let mut labels: Vec<String> = state
            .items
            .iter()
            .flat_map(|item| item.tags.iter().cloned())
            .collect();
        labels.sort();
        labels.dedup();
        state.catalog_cache = Some(labels.clone());
        labels
    }

    /// A monotonic counter that advances on every mutation altering the live set
    /// (`store`-created, `update`, `forget`). A reader can cache output derived
    /// from the store and rebuild only when this changes — never decreases, so an
    /// equal value means the catalog/items are unchanged since the last read.
    pub fn version(&self) -> u64 {
        self.lock().version
    }

    fn lock(&self) -> MutexGuard<'_, State> {
        self.inner
            .state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Append one serialized record to the journal.
    ///
    /// Best-effort at the storage layer: the in-memory state stays authoritative
    /// on failure, but the error is returned so the caller can record it in
    /// [`State::last_persist_error`] rather than silently dropping it. A
    /// serialize failure (a bug — records always serialize) is reported as a
    /// source-preserving [`FsError::Io`].
    fn append_record(&self, record: &Record) -> Result<(), FsError> {
        let mut line = serde_json::to_vec(record)
            .map_err(|error| FsError::io(LongTermPersistError::SerializeRecord(error)))?;
        line.push(b'\n');
        F::append(&self.inner.path, &line)
    }

    /// Rewrite the journal from the live set when dead lines pass the threshold.
    ///
    /// Records any write failure in [`State::last_persist_error`] and leaves the
    /// dead count untouched so a later mutation retries the rewrite.
    fn maybe_compact(&self, state: &mut State) {
        if state.dead < DEFAULT_COMPACT_DEAD_THRESHOLD {
            return;
        }
        let mut buffer = Vec::new();
        for item in &state.items {
            match serde_json::to_vec(&Record::Put(item.clone())) {
                Ok(mut line) => {
                    line.push(b'\n');
                    buffer.extend_from_slice(&line);
                }
                Err(error) => {
                    log::warn!(
                        "long-term memory {}: compaction serialize failed: {error}",
                        self.inner.path
                    );
                    state.last_persist_error =
                        Some(FsError::io(LongTermPersistError::CompactSerialize(error)));
                    return;
                }
            }
        }
        match F::write_atomic(&self.inner.path, &buffer) {
            Ok(()) => {
                state.dead = 0;
                state.last_persist_error = None;
            }
            Err(error) => {
                log::warn!(
                    "long-term memory {}: compaction write failed: {error}",
                    self.inner.path
                );
                state.last_persist_error = Some(error);
            }
        }
    }
}

/// Journal path for a store directory.
fn journal_path(dir: &str) -> String {
    format!("{}/{}", dir.trim_end_matches('/'), RECORDS_FILE)
}

/// Replay the journal into the live set.
///
/// A missing journal ([`FsError::NotFound`]) yields an empty state; any other
/// read failure is returned as an error so a genuine I/O fault is not silently
/// mistaken for an empty store. A torn trailing line (crash mid-append) still
/// fails to parse and is skipped without aborting the replay.
fn load_state<F: ClawFs>(path: &str) -> Result<State, FsError> {
    let mut state = State::default();
    let bytes = match F::read(path) {
        Ok(bytes) => bytes,
        Err(FsError::NotFound) => return Ok(state),
        Err(error) => return Err(error),
    };
    for line in bytes.split(|byte| *byte == b'\n') {
        if line.is_empty() {
            continue;
        }
        match serde_json::from_slice::<Record>(line) {
            Ok(Record::Put(item)) => {
                state.next_seq = state.next_seq.max(item.seq.saturating_add(1));
                if let Some(existing) = state.items.iter_mut().find(|live| live.id == item.id) {
                    *existing = item;
                    state.dead = state.dead.saturating_add(1);
                } else {
                    state.items.push(item);
                }
            }
            Ok(Record::Del { id }) => {
                if let Some(position) = state.items.iter().position(|live| live.id == id) {
                    state.items.remove(position);
                }
                state.dead = state.dead.saturating_add(1);
            }
            // A torn trailing line (crash mid-append) won't parse; skip it.
            Err(_) => {}
        }
    }
    Ok(state)
}

/// Normalize content for dedup: lowercase, collapse runs of whitespace.
fn normalize(content: &str) -> String {
    content
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .to_lowercase()
}

/// Whether an item's content or keywords contain `needle` (already lowercased).
fn matches_query(item: &MemoryItem, needle: &str) -> bool {
    item.content.to_lowercase().contains(needle)
        || item
            .keywords
            .iter()
            .any(|keyword| keyword.to_lowercase().contains(needle))
}
