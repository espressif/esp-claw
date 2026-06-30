//! `TranscriptStore` — the agent's complete, append-only conversation record.
//!
//! This is the **source of truth** for what was said: every committed turn, kept
//! verbatim, forever (within the store's own lifetime). Its natural unit is a
//! **turn**: one turn's worth of messages, produced together by the agent loop:
//!
//! ```text
//! turn {                        // one group, one [`TurnId`]
//!   user message
//!   assistant message (text and/or tool_calls)
//!   tool result(s)
//!   assistant message
//! }
//! ```
//!
//! The store owns, oldest-to-newest:
//! - `turns` — committed turns kept verbatim, each stamped with a monotonic
//!   [`TurnId`] so its chronological position is stable across reloads.
//! - the **open turn** — the in-progress turn (see [`GroupGuard`]), volatile and
//!   not yet stamped.
//!
//! # It does not compact
//!
//! Compaction — summarizing an aged prefix so the conversation fits the model's
//! context window — is **not** this store's job. Compaction is a property of the
//! *LLM request*, not of the record: the summary is a derived artifact assembled
//! at request time by the agent layer's rolling-summary context adapter, which
//! *reads* this store ([`turns_snapshot`](TranscriptStore::turns_snapshot)) and
//! keeps its own summary. This store never deletes a turn, never folds turns into
//! a summary, and has no token budget. It just stores and replays the verbatim
//! transcript. (Bounding on-disk growth, if ever needed, is a separate retention
//! concern — also not compaction.)
//!
//! # Turns are built through a guard
//!
//! Conversation content is appended through a [`GroupGuard`] from
//! [`group`](TranscriptStore::group). The guard buffers the open turn and commits
//! it as a single record when it drops, so a turn can never be left half-open and
//! a reader never sees a partial turn boundary.
//!
//! # Threading
//!
//! All state mutation (appends, commits, persistence) happens on the
//! **foreground** thread that owns the store. A single store must be driven from
//! one thread; the `Arc`-backed state lets a reader (a context adapter) hold a
//! clone for its read snapshots.
//!
//! # Identity and persistence
//!
//! Each store is keyed by a `conversation_id`. Two files under
//! [`TranscriptConfig::dir`]:
//!
//! - `conversation-<id>.jsonl` — the **data log**: one JSON record per line
//!   (`group`), **append-only**. The source of truth.
//! - `conversation-<id>.json` — the **index manifest**: a rebuildable cache
//!   listing the byte `(off, len)` of every record plus `covered_len` and
//!   `next_id`, rewritten atomically as turns are appended.

use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use claw_interface::{ClawFile, ClawFs};

/// Minimum gap between persistence writes (flash-wear debounce).
const DEFAULT_PERSIST_DEBOUNCE: Duration = Duration::from_secs(5);
/// Per-conversation filenames: `{dir}/{FILE_PREFIX}{id}{DATA_EXT|INDEX_EXT}`.
const FILE_PREFIX: &str = "conversation-";
const DATA_EXT: &str = ".jsonl";
const INDEX_EXT: &str = ".json";
/// Manifest schema version, so a future layout change can be detected on load.
const MANIFEST_VERSION: u32 = 1;

/// A monotonic logical identifier for a turn.
///
/// Fixes chronological order and gives compaction (in the agent layer) a stable
/// handle for "the summary covers turns up to here". A distinct type from byte
/// offsets/lengths so the two can never be swapped: ordering is keyed on
/// `TurnId`, addressing on [`ByteOffset`].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct TurnId(pub u64);

impl TurnId {
    /// The id that immediately follows this one.
    fn next(self) -> TurnId {
        TurnId(self.0.saturating_add(1))
    }
}

/// A byte position within the data log. Addressing only — never compared for
/// chronology (that is [`TurnId`]'s job).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(transparent)]
struct ByteOffset(usize);

impl ByteOffset {
    /// The offset `len` bytes further on.
    fn advance(self, len: ByteLen) -> ByteOffset {
        ByteOffset(self.0.saturating_add(len.0))
    }
    /// As the `u64` the [`ClawFs`] read API expects (widening, lossless).
    fn as_u64(self) -> u64 {
        self.0 as u64
    }
    /// This position viewed as the length of the region `[0, self)`.
    fn as_len(self) -> ByteLen {
        ByteLen(self.0)
    }
}

/// A number of bytes: one record line, the whole data log, a covered prefix, …
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(transparent)]
struct ByteLen(usize);

impl ByteLen {
    /// The length of a serialized line.
    fn of(bytes: &[u8]) -> ByteLen {
        ByteLen(bytes.len())
    }
    /// As the `usize` the [`ClawFs`] read API expects.
    fn as_usize(self) -> usize {
        self.0
    }
    /// As the starting position just past a region of this size.
    fn as_offset(self) -> ByteOffset {
        ByteOffset(self.0)
    }
    fn saturating_sub(self, other: ByteLen) -> ByteLen {
        ByteLen(self.0.saturating_sub(other.0))
    }
    /// From a [`ClawFs::len`] result, clamping if it somehow exceeds `usize` (a
    /// 32-bit device can only address `usize` bytes; conversation files are tiny).
    fn from_file_len(len: u64) -> ByteLen {
        ByteLen(usize::try_from(len).unwrap_or(usize::MAX))
    }
}

/// Tuning for a [`TranscriptStore`].
///
/// Construct with [`TranscriptConfig::new`] for sensible defaults, then override
/// individual fields as needed. There are deliberately **no** token budgets here:
/// the store does not compact, so it has nothing to tune about summarization.
///
/// # Examples
///
/// ```
/// use std::time::Duration;
///
/// use claw_memory::TranscriptConfig;
///
/// // Defaults, then disable the write debounce.
/// let config = TranscriptConfig {
///     persist_debounce: Duration::ZERO,
///     ..TranscriptConfig::new("/data/conversations")
/// };
///
/// assert_eq!(config.dir, "/data/conversations");
/// ```
pub struct TranscriptConfig {
    /// Base directory for per-conversation files, already resolved against the
    /// DATA root by the caller. The filenames are derived from the conversation id.
    pub dir: String,
    /// Minimum interval between filesystem writes.
    pub persist_debounce: Duration,
}

impl TranscriptConfig {
    /// Config for conversation files under `dir`, with default tuning otherwise.
    ///
    /// `dir` is the base directory for the per-conversation files; the filenames
    /// themselves are derived from the conversation id. Accepts anything
    /// `Into<String>` (`&str`, `String`, …).
    ///
    /// # Examples
    ///
    /// ```
    /// use claw_memory::TranscriptConfig;
    ///
    /// let config = TranscriptConfig::new("/data/conversations");
    /// assert_eq!(config.dir, "/data/conversations");
    /// ```
    pub fn new(dir: impl Into<String>) -> Self {
        Self {
            dir: dir.into(),
            persist_debounce: DEFAULT_PERSIST_DEBOUNCE,
        }
    }
}

/// One committed turn, lent read-only to context adapters.
///
/// Returned (as a shared snapshot) by
/// [`turns_snapshot`](TranscriptStore::turns_snapshot). Carries the turn's stable
/// [`TurnId`] alongside its verbatim messages, so an adapter computing a
/// summarization boundary or a recent-tail cutoff can reason about *which* turns
/// it has covered. The volatile open turn is **not** here (see
/// [`open_turn_messages`](TranscriptStore::open_turn_messages)).
#[derive(Clone, Debug)]
pub struct Turn {
    /// This turn's stable chronological id.
    pub id: TurnId,
    /// The turn's messages, oldest-to-newest.
    pub messages: Vec<Value>,
}

/// One record as stored on a line of the data `.jsonl`.
#[derive(Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
enum LogRecord {
    /// A committed turn: its messages in order.
    Group { id: TurnId, msgs: Vec<Value> },
}

/// One record's location inside the data `.jsonl`, as stored in the manifest.
#[derive(Clone, Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
enum IndexEntry {
    Group {
        off: ByteOffset,
        len: ByteLen,
        id: TurnId,
    },
}

/// The index manifest: the layout of the data log, rewritten atomically.
#[derive(Default, Serialize, Deserialize)]
struct Manifest {
    version: u32,
    /// Data-log byte length this manifest describes; load tail-scans past it.
    covered_len: ByteLen,
    next_id: TurnId,
    live: Vec<IndexEntry>,
}

/// A committed turn plus its byte location in the data log (once flushed).
struct StoredGroup {
    id: TurnId,
    msgs: Vec<Value>,
    loc: Option<(ByteOffset, ByteLen)>,
}

/// A serialized data line awaiting its append, tagged with the turn it belongs
/// to so its `loc` can be written back once appended.
struct Pending {
    line: Vec<u8>,
    id: TurnId,
}

/// The lock-protected contents of the store.
#[derive(Default)]
struct StoreState {
    groups: Vec<StoredGroup>,
    /// The in-progress turn's messages — not yet committed (volatile, no id).
    open_group: Vec<Value>,
    next_id: TurnId,

    /// Records appended in memory but not yet written to the `.jsonl`.
    pending: Vec<Pending>,
    /// Current byte length of the `.jsonl` (also the next append offset).
    data_len: ByteLen,
    /// Data length the on-disk manifest currently describes.
    manifest_covered_len: ByteLen,

    /// Cached flat snapshot returned by [`TranscriptStore::messages`] (committed
    /// turns + open turn, in order). Rebuilt lazily and shared as an `Arc`;
    /// invalidated whenever content changes.
    messages_cache: Option<Arc<Value>>,
    /// Cached committed-turn snapshot returned by
    /// [`TranscriptStore::turns_snapshot`]. Rebuilt lazily and shared as an `Arc`;
    /// invalidated whenever content changes.
    turns_cache: Option<Arc<Vec<Turn>>>,
    /// Monotonic content version, bumped whenever either cache is invalidated (an
    /// open-turn append or a commit). A pull-based reader caches work keyed on
    /// this and recomputes only when it advances — see
    /// [`TranscriptStore::version`].
    version: u64,

    last_persist: Option<Instant>,
}

impl StoreState {
    /// Content changed: drop both cached snapshots and bump the version so
    /// pull-based readers (and the next `messages`/`turns_snapshot`) rebuild.
    fn mark_changed(&mut self) {
        self.messages_cache = None;
        self.turns_cache = None;
        self.version = self.version.saturating_add(1);
    }
}

/// Shared inner state — held behind an `Arc` so a context adapter can keep its
/// own clone of the store and read the same transcript the agent writes.
struct StoreInner<F: ClawFs + 'static> {
    conversation_id: usize,
    data_path: String,
    index_path: String,
    state: Mutex<StoreState>,
    config: TranscriptConfig,
    /// Persistence backend (read on construction, written on the foreground).
    fs: F,
}

/// The agent's complete conversation transcript: an append-only, verbatim record
/// of every turn. See the module docs for the storage layout.
///
/// Build one with [`new`](Self::new), append turns through the [`GroupGuard`]
/// returned by [`group`](Self::group), read the model-ready transcript with
/// [`messages`](Self::messages) (or the turn-structured
/// [`turns_snapshot`](Self::turns_snapshot)), and checkpoint with
/// [`flush`](Self::flush). Drive a single store from one thread.
///
/// # Examples
///
/// ```
/// # use claw_interface::MemFs;
/// # use claw_memory::{TranscriptConfig, TranscriptStore};
/// let mut store = TranscriptStore::new(
///     42,
///     TranscriptConfig::new("/data/conversations"),
///     MemFs::new(),
/// );
///
/// // One turn = one `group()`; the whole turn commits when the guard drops.
/// {
///     let turn = store.group();
///     turn.append_user("what's the weather?");
///     turn.append_assistant(r#"{"role":"assistant","content":"Sunny."}"#);
/// }
///
/// let rendered = store.messages();
/// assert_eq!(rendered.as_array().map(|m| m.len()), Some(2));
/// store.flush(); // checkpoint, e.g. on a clean shutdown
/// ```
pub struct TranscriptStore<F: ClawFs + 'static> {
    inner: Arc<StoreInner<F>>,
}

// Manual `Clone`: only the `Arc` is cloned, so this is cheap and does **not**
// require `F: Clone` (a `#[derive(Clone)]` would wrongly add that bound).
impl<F: ClawFs + 'static> Clone for TranscriptStore<F> {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl<F: ClawFs + 'static> TranscriptStore<F> {
    /// Build the store for `conversation_id`, restoring its persisted contents if
    /// present (missing or unreadable files start empty).
    ///
    /// Different ids map to different files under [`TranscriptConfig::dir`], so
    /// each conversation is stored independently. A mismatched or unreadable index
    /// is rebuilt from the data log during construction.
    ///
    /// # Examples
    ///
    /// ```
    /// # use claw_interface::MemFs;
    /// # use claw_memory::{TranscriptConfig, TranscriptStore};
    /// let store = TranscriptStore::new(
    ///     7,
    ///     TranscriptConfig::new("/data/conversations"),
    ///     MemFs::new(),
    /// );
    /// assert_eq!(store.conversation_id(), 7);
    /// assert!(store.messages().as_array().unwrap().is_empty()); // missing files start empty
    /// ```
    pub fn new(conversation_id: usize, config: TranscriptConfig, fs: F) -> Self {
        let data_path = conversation_path(&config.dir, conversation_id, DATA_EXT);
        let index_path = conversation_path(&config.dir, conversation_id, INDEX_EXT);
        let (mut state, needs_rebuild) = load_state(&fs, &data_path, &index_path);
        if needs_rebuild {
            write_live_set_to_files(&fs, &data_path, &index_path, &mut state, conversation_id);
        }
        Self {
            inner: Arc::new(StoreInner {
                conversation_id,
                data_path,
                index_path,
                state: Mutex::new(state),
                config,
                fs,
            }),
        }
    }

    /// This store's conversation id.
    pub fn conversation_id(&self) -> usize {
        self.inner.conversation_id
    }

    /// A monotonic counter bumped whenever the transcript content changes (an
    /// open-turn append or a turn commit).
    ///
    /// Lets a pull-based reader cache output keyed on the transcript and rebuild
    /// only when this advances, without diffing [`messages`](Self::messages) or
    /// [`turns_snapshot`](Self::turns_snapshot).
    pub fn version(&self) -> u64 {
        self.lock_state().version
    }

    /// Open a turn. Append its messages through the returned [`GroupGuard`]; the
    /// whole turn is committed as one group when the guard drops.
    ///
    /// Takes `&self` (the open turn buffers in the `Arc`-backed state), but a
    /// single store must be driven from one thread.
    pub fn group(&self) -> GroupGuard<F> {
        GroupGuard {
            inner: Arc::clone(&self.inner),
        }
    }

    /// The current messages, ready to send to the model in chronological order:
    /// every committed turn's messages followed by the in-progress open turn.
    /// Returns a shared, internally consistent JSON array snapshot.
    ///
    /// This is the **full verbatim transcript** — the source of truth, with no
    /// summarization applied (summarization happens in the agent layer at request
    /// time, never here). The snapshot is cached and shared as an `Arc`: repeated
    /// calls between mutations return a cheap refcount bump rather than
    /// rebuilding/cloning the transcript.
    pub fn messages(&self) -> Arc<Value> {
        let mut state = self.lock_state();
        if let Some(cached) = &state.messages_cache {
            return Arc::clone(cached);
        }
        let mut out = Vec::new();
        for group in &state.groups {
            out.extend(group.msgs.iter().cloned());
        }
        out.extend(state.open_group.iter().cloned());
        let snapshot = Arc::new(Value::Array(out));
        state.messages_cache = Some(Arc::clone(&snapshot));
        snapshot
    }

    /// A turn-structured snapshot of the **committed** turns, oldest-to-newest,
    /// each carrying its stable [`TurnId`].
    ///
    /// This is what a context adapter reads to reason about turn boundaries (e.g.
    /// "summarize turns up to id N", "keep the newest turns verbatim"). The
    /// volatile open turn is excluded — read it separately with
    /// [`open_turn_messages`](Self::open_turn_messages). Cached and shared as an
    /// `Arc`; gate calls on [`version`](Self::version) to rebuild only when the
    /// transcript changed.
    pub fn turns_snapshot(&self) -> Arc<Vec<Turn>> {
        let mut state = self.lock_state();
        if let Some(cached) = &state.turns_cache {
            return Arc::clone(cached);
        }
        let turns: Vec<Turn> = state
            .groups
            .iter()
            .map(|group| Turn {
                id: group.id,
                messages: group.msgs.clone(),
            })
            .collect();
        let snapshot = Arc::new(turns);
        state.turns_cache = Some(Arc::clone(&snapshot));
        snapshot
    }

    /// The in-progress open turn's messages, oldest-to-newest (empty when no turn
    /// is open).
    ///
    /// Volatile and unstamped: it is not part of [`turns_snapshot`](Self::turns_snapshot)
    /// (which is committed turns only) but a reader rendering the verbatim recent
    /// tail must include it, since it is always the newest content.
    pub fn open_turn_messages(&self) -> Vec<Value> {
        self.lock_state().open_group.clone()
    }

    /// Force pending changes to disk now (ignoring the debounce) and refresh the
    /// index manifest.
    ///
    /// Persists only **committed** turns; an open turn is committed when its
    /// [`GroupGuard`] drops. The clean-shutdown order is therefore "drop the
    /// guard, then `flush`". This is the manual, immediate form of the automatic
    /// debounced persistence — same writes, just now.
    pub fn flush(&self) {
        persist(&self.inner, true);
    }

    fn lock_state(&self) -> MutexGuard<'_, StoreState> {
        lock_state(&self.inner)
    }
}

/// An open turn. Append messages through it; the turn is committed as one group
/// record when the guard drops (or on an explicit [`commit`](Self::commit)).
///
/// Obtained from [`TranscriptStore::group`]. Holds an `Arc` into the store's
/// inner state so it carries no lifetime and can be stored across async
/// boundaries or inside structs like `BaseAgent`. Only one turn should be open at
/// a time per store instance; behaviour is unspecified if two live `GroupGuard`s
/// share the same store (their messages interleave in the single `open_group`
/// buffer).
///
/// # Examples
///
/// ```
/// # use claw_interface::MemFs;
/// # use claw_memory::{TranscriptConfig, TranscriptStore};
/// # use serde_json::json;
/// # let store = TranscriptStore::new(1, TranscriptConfig::new("/data/conversations"), MemFs::new());
/// let turn = store.group();
/// turn.append_user("call the weather tool");
/// turn.append_patch(&json!([
///     { "role": "assistant", "content": "calling tool" },
///     { "role": "tool", "tool_call_id": "c1", "content": "{\"temp_c\":21}" },
/// ]));
/// // Reads see the open turn before it commits.
/// assert_eq!(store.messages().as_array().map(|m| m.len()), Some(3));
/// turn.commit(); // or just let it drop
/// ```
pub struct GroupGuard<F: ClawFs + 'static> {
    inner: Arc<StoreInner<F>>,
}

impl<F: ClawFs + 'static> GroupGuard<F> {
    /// Append a user (or addon) message to the open turn.
    pub fn append_user(&self, content: impl Into<String>) {
        self.push_open(json!({ "role": "user", "content": content.into() }));
    }

    /// Append a raw assistant message (plain text and/or `tool_calls`).
    ///
    /// `raw_message_json` is the backend-shaped assistant message object; an
    /// unparseable value is logged and dropped rather than corrupting the turn.
    pub fn append_assistant(&self, raw_message_json: &str) {
        match serde_json::from_str::<Value>(raw_message_json) {
            Ok(message) => self.push_open(message),
            Err(err) => log::warn!(
                "conversation {}: invalid assistant json: {err}",
                self.inner.conversation_id
            ),
        }
    }

    /// Append a tool result for the call `tool_call_id` to the open turn.
    ///
    /// Set `is_error` when the tool failed, so the model can see the call did not
    /// succeed.
    pub fn append_tool_result(&self, tool_call_id: &str, content: &str, is_error: bool) {
        self.push_open(json!({
            "role": "tool",
            "tool_call_id": tool_call_id,
            "content": content,
            "is_error": is_error,
        }));
    }

    /// Append a whole batch of messages to the open turn (e.g. one tool round).
    ///
    /// `messages` must be a JSON array; a non-array value is logged and ignored.
    pub fn append_patch(&self, messages: &Value) {
        let Some(items) = messages.as_array() else {
            log::warn!(
                "conversation {}: append_patch expected a JSON array",
                self.inner.conversation_id
            );
            return;
        };
        for message in items {
            self.push_open(message.clone());
        }
    }

    /// Commit the open turn now as one group record, then persist if due.
    pub fn commit(&self) {
        let inner = &self.inner;
        let due = {
            let mut state = lock_state(inner);
            if state.open_group.is_empty() {
                return;
            }
            let msgs = std::mem::take(&mut state.open_group);
            let id = next_id(&mut state);
            enqueue(&mut state, id, msgs.clone(), inner.conversation_id);
            state.groups.push(StoredGroup {
                id,
                msgs,
                loc: None,
            });
            // A committed turn changes the turn-structured snapshot (a new turn
            // appears), so invalidate caches and bump the version.
            state.mark_changed();
            persist_due(inner, &state)
        };
        if due {
            persist(inner, false);
        }
    }

    fn push_open(&self, message: Value) {
        let mut state = lock_state(&self.inner);
        state.open_group.push(message);
        state.mark_changed();
    }
}

impl<F: ClawFs + 'static> Drop for GroupGuard<F> {
    fn drop(&mut self) {
        self.commit();
    }
}

/// Allocate the next monotonic id.
fn next_id(state: &mut StoreState) -> TurnId {
    let id = state.next_id;
    state.next_id = id.next();
    id
}

/// Serialize a group record to a data line and queue it for the next append.
fn enqueue(state: &mut StoreState, id: TurnId, msgs: Vec<Value>, conversation_id: usize) {
    let record = LogRecord::Group { id, msgs };
    match serde_json::to_vec(&record) {
        Ok(mut line) => {
            line.push(b'\n');
            state.pending.push(Pending { line, id });
        }
        Err(err) => log::warn!("conversation {conversation_id}: serialize record failed: {err}"),
    }
}

/// Flush pending records (one `append`) and, when needed, rewrite the manifest.
fn persist<F: ClawFs + 'static>(inner: &StoreInner<F>, force_manifest: bool) {
    let mut state = lock_state(inner);
    // Pending data always makes the manifest stale: after the append the data log
    // has records the index doesn't know about. Fold that into want_manifest so
    // the two files stay in sync on every write.
    let has_pending = !state.pending.is_empty();
    let want_manifest = force_manifest || has_pending;
    if !want_manifest {
        return;
    }

    if !state.pending.is_empty() {
        let mut data_buf = Vec::new();
        let mut locs = Vec::with_capacity(state.pending.len());
        let mut off = state.data_len.as_offset();
        for pending in &state.pending {
            let len = ByteLen::of(&pending.line);
            data_buf.extend_from_slice(&pending.line);
            locs.push((pending.id, off, len));
            off = off.advance(len);
        }
        if let Err(err) = inner.fs.append(&inner.data_path, &data_buf) {
            log::warn!(
                "conversation {}: data append failed: {err}",
                inner.conversation_id
            );
            return;
        }
        state.data_len = off.as_len();
        for (id, off, len) in locs {
            set_loc(&mut state, id, off, len);
        }
        state.pending.clear();
    }
    state.last_persist = Some(Instant::now());

    if let Some(bytes) = build_manifest_bytes(&state, inner.conversation_id) {
        match inner.fs.write_atomic(&inner.index_path, &bytes) {
            Ok(()) => state.manifest_covered_len = state.data_len,
            Err(err) => log::warn!(
                "conversation {}: index write failed: {err}",
                inner.conversation_id
            ),
        }
    }
}

/// Rewrite `.jsonl` + `.json` from the in-memory turns in id order, updating
/// state locs to the new layout on success.
fn write_live_set_to_files(
    fs: &impl ClawFs,
    data_path: &str,
    index_path: &str,
    state: &mut StoreState,
    conversation_id: usize,
) {
    let mut data_buf = Vec::new();
    let mut live = Vec::new();
    let mut locs: Vec<(TurnId, ByteOffset, ByteLen)> = Vec::new();
    let mut off = ByteOffset::default();

    for group in &state.groups {
        let record = LogRecord::Group {
            id: group.id,
            msgs: group.msgs.clone(),
        };
        let Some(len) = append_line(&mut data_buf, &record, conversation_id) else {
            return;
        };
        live.push(IndexEntry::Group {
            off,
            len,
            id: group.id,
        });
        locs.push((group.id, off, len));
        off = off.advance(len);
    }

    let manifest = Manifest {
        version: MANIFEST_VERSION,
        covered_len: off.as_len(),
        next_id: state.next_id,
        live,
    };
    let manifest_bytes = match serde_json::to_vec(&manifest) {
        Ok(bytes) => bytes,
        Err(err) => {
            log::warn!(
                "conversation {conversation_id}: write_live manifest serialize failed: {err}"
            );
            return;
        }
    };

    if let Err(err) = fs.write_atomic(data_path, &data_buf) {
        log::warn!("conversation {conversation_id}: write_live data write failed: {err}");
        return;
    }
    if let Err(err) = fs.write_atomic(index_path, &manifest_bytes) {
        log::warn!("conversation {conversation_id}: write_live index write failed: {err}");
        // Data file is the fresh truth; stale manifest is rebuilt on next load.
    }

    state.pending.clear();
    state.data_len = off.as_len();
    state.manifest_covered_len = off.as_len();
    for group in &mut state.groups {
        group.loc = None;
    }
    for (id, off, len) in locs {
        set_loc(state, id, off, len);
    }
}

/// Serialize `record` into `buf` with a trailing newline; returns the line length.
fn append_line(buf: &mut Vec<u8>, record: &LogRecord, conversation_id: usize) -> Option<ByteLen> {
    match serde_json::to_vec(record) {
        Ok(mut line) => {
            line.push(b'\n');
            let len = ByteLen::of(&line);
            buf.extend_from_slice(&line);
            Some(len)
        }
        Err(err) => {
            log::warn!("conversation {conversation_id}: serialize record failed: {err}");
            None
        }
    }
}

/// Record a flushed group's byte location back onto its in-memory entry.
fn set_loc(state: &mut StoreState, id: TurnId, off: ByteOffset, len: ByteLen) {
    if let Some(group) = state.groups.iter_mut().find(|g| g.id == id) {
        group.loc = Some((off, len));
    }
}

/// Build the manifest of the current turns (those already on disk).
fn build_manifest_bytes(state: &StoreState, conversation_id: usize) -> Option<Vec<u8>> {
    let mut live = Vec::new();
    for group in &state.groups {
        if let Some((off, len)) = group.loc {
            live.push(IndexEntry::Group {
                off,
                len,
                id: group.id,
            });
        }
    }
    let manifest = Manifest {
        version: MANIFEST_VERSION,
        covered_len: state.data_len,
        next_id: state.next_id,
        live,
    };
    match serde_json::to_vec(&manifest) {
        Ok(bytes) => Some(bytes),
        Err(err) => {
            log::warn!("conversation {conversation_id}: manifest serialize failed: {err}");
            None
        }
    }
}

fn persist_due<F: ClawFs + 'static>(inner: &StoreInner<F>, state: &StoreState) -> bool {
    if state.pending.is_empty() {
        return false;
    }
    match state.last_persist {
        None => true,
        Some(at) => at.elapsed() >= inner.config.persist_debounce,
    }
}

/// Return true if `record` is the type and id that `entry` claims.
fn verify_entry(entry: &IndexEntry, record: &LogRecord) -> bool {
    match (entry, record) {
        (IndexEntry::Group { id: eid, .. }, LogRecord::Group { id: rid, .. }) => eid == rid,
    }
}

/// Load and rehydrate persisted state. Returns `(state, needs_rebuild)`.
/// `needs_rebuild` is true when a manifest existed but its entries did not match
/// the data log — the caller should rewrite both files from the recovered state.
fn load_state(fs: &impl ClawFs, data_path: &str, index_path: &str) -> (StoreState, bool) {
    let mut state = StoreState::default();
    let mut covered_len = ByteLen::default();
    let mut manifest_next_id = TurnId::default();
    let mut mismatch = false;

    // One handle to the data log, reused for every indexed record read and the
    // tail scan below, instead of reopening the file per access.
    let mut data_file = fs.open(data_path).ok();

    if let Ok(bytes) = fs.read(index_path) {
        if let Ok(manifest) = serde_json::from_slice::<Manifest>(&bytes) {
            covered_len = manifest.covered_len;
            manifest_next_id = manifest.next_id;
            'entries: for entry in &manifest.live {
                let (off, len) = entry_loc(entry);
                let Some(file) = data_file.as_mut() else {
                    // The manifest references a data log that cannot be opened;
                    // rebuild from whatever the tail scan recovers.
                    mismatch = true;
                    break 'entries;
                };
                match file.read_exact_at(off.as_u64(), len.as_usize()) {
                    Ok(buf) => match parse_record(&buf) {
                        Some(record) if verify_entry(entry, &record) => {
                            apply_record(&mut state, record, Some((off, len)));
                        }
                        Some(_) => {
                            log::error!(
                                "conversation load: manifest entry at offset {} does not match \
                                 data log record; rebuilding",
                                off.as_u64()
                            );
                            mismatch = true;
                            break 'entries;
                        }
                        None => {
                            log::error!(
                                "conversation load: manifest entry at offset {} could not be \
                                 parsed; rebuilding",
                                off.as_u64()
                            );
                            mismatch = true;
                            break 'entries;
                        }
                    },
                    Err(err) => {
                        log::error!(
                            "conversation load: manifest entry at offset {} could not be read: \
                             {err}; rebuilding",
                            off.as_u64()
                        );
                        mismatch = true;
                        break 'entries;
                    }
                }
            }
        }
    }

    if mismatch {
        state = StoreState::default();
        covered_len = ByteLen::default();
        manifest_next_id = TurnId::default();
    }

    let data_len = ByteLen::from_file_len(
        data_file
            .as_ref()
            .and_then(|file| file.size().ok())
            .unwrap_or(0),
    );
    if data_len > covered_len {
        let extra = data_len.saturating_sub(covered_len);
        let tail = data_file.as_mut().and_then(|file| {
            file.read_exact_at(covered_len.as_offset().as_u64(), extra.as_usize())
                .ok()
        });
        if let Some(tail) = tail {
            scan_tail(&mut state, &tail, covered_len.as_offset());
        }
    }

    state.data_len = data_len;
    state.manifest_covered_len = covered_len;
    state.groups.sort_by_key(|g| g.id);
    state.next_id = manifest_next_id.max(max_seen_id(&state).next());
    (state, mismatch)
}

/// Parse a newline-delimited tail buffer, applying each complete record.
fn scan_tail(state: &mut StoreState, tail: &[u8], base_off: ByteOffset) {
    let mut start = 0usize;
    let mut pos = base_off;
    for (i, byte) in tail.iter().enumerate() {
        if *byte != b'\n' {
            continue;
        }
        let line = tail.get(start..i).unwrap_or(&[]);
        let line_len = ByteLen(i.saturating_sub(start).saturating_add(1));
        if let Some(record) = parse_record(line) {
            apply_record(state, record, Some((pos, line_len)));
        }
        pos = pos.advance(line_len);
        start = i.saturating_add(1);
    }
}

/// Fold one decoded record into the live state.
fn apply_record(state: &mut StoreState, record: LogRecord, loc: Option<(ByteOffset, ByteLen)>) {
    match record {
        LogRecord::Group { id, msgs } => {
            state.groups.push(StoredGroup { id, msgs, loc });
        }
    }
}

/// Highest committed turn id seen.
fn max_seen_id(state: &StoreState) -> TurnId {
    state.groups.iter().map(|g| g.id).max().unwrap_or_default()
}

fn entry_loc(entry: &IndexEntry) -> (ByteOffset, ByteLen) {
    match *entry {
        IndexEntry::Group { off, len, .. } => (off, len),
    }
}

fn parse_record(bytes: &[u8]) -> Option<LogRecord> {
    if bytes.is_empty() {
        return None;
    }
    match serde_json::from_slice::<LogRecord>(bytes) {
        Ok(record) => Some(record),
        Err(err) => {
            log::warn!("conversation load: skipping unparseable record: {err}");
            None
        }
    }
}

/// Build a per-conversation path from the base dir, id, and extension.
fn conversation_path(dir: &str, conversation_id: usize, ext: &str) -> String {
    format!(
        "{}/{FILE_PREFIX}{conversation_id}{ext}",
        dir.trim_end_matches('/')
    )
}

fn lock_state<F: ClawFs + 'static>(inner: &StoreInner<F>) -> MutexGuard<'_, StoreState> {
    inner
        .state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
