//! `TranscriptStore` — the agent's complete, append-only transcript record.
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
//! Compaction — summarizing an aged prefix so the transcript fits the model's
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
//! Transcript content is appended through either:
//! - a [`GroupGuard`] from [`group`](TranscriptStore::group), for RAII turn
//!   grouping; or
//! - the direct `push_*` methods plus [`commit_open_turn`](TranscriptStore::commit_open_turn),
//!   for an agent loop that already owns the turn lifecycle.
//!
//! Both paths write the same open turn and commit it as a single record, so readers
//! see one transcript regardless of the writer style.
//! A hard cancellation can instead call
//! [`discard_open_turn`](TranscriptStore::discard_open_turn) to drop the volatile
//! open turn without committing it.
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
//! Each store is keyed by a `transcript_id`. A **persisting** store (built with
//! [`TranscriptStore::new`]) keeps two files under the `dir` it is given:
//!
//! - `<id>.jsonl` — the **data log**: one JSON record per line (`group`),
//!   **append-only**. The source of truth.
//! - `<id>.json` — the **index manifest**: a rebuildable cache listing the byte
//!   `(off, len)` of every record plus `covered_len` and `next_id`, rewritten
//!   atomically as turns are appended.
//!
//! An **in-memory** store (built with [`TranscriptStore::in_memory`]) holds the
//! same transcript but never touches the filesystem: it starts empty and every
//! persist is a no-op. Subagents use it — their transcripts are context-management
//! scratch that is never enumerated or resumed, so it need not survive a restart.

use std::marker::PhantomData;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use claw_interface::{ClawFile, ClawFs, FsError};

/// Minimum gap between persistence writes (flash-wear debounce).
const DEFAULT_PERSIST_DEBOUNCE: Duration = Duration::from_secs(5);
/// Per-transcript filenames: `{dir}/{id}{DATA_EXT|INDEX_EXT}`.
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
#[derive(
    Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize,
)]
#[serde(transparent)]
pub struct TurnId(pub u64);

impl TurnId {
    /// Wrap a raw turn number. Named `new` so [`TurnIdAllocator`] can construct
    /// ids generically.
    const fn new(value: u64) -> Self {
        TurnId(value)
    }

    /// The id that immediately follows this one.
    fn next(self) -> TurnId {
        TurnId(self.0.saturating_add(1))
    }
}

claw_utils::define_id_allocator!(
    /// Hands out this transcript's [`TurnId`]s. Single-owner (a `StoreState`
    /// field mutated under the store lock), so the lock it needs is that outer
    /// one, not one of its own. Its position is persisted via
    /// [`peek`](TurnIdAllocator::peek) into the manifest and restored with
    /// [`starting_at`](TurnIdAllocator::starting_at).
    TurnIdAllocator(TurnId),
    TurnId(0)
);

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
    /// 32-bit device can only address `usize` bytes; transcript files are tiny).
    fn from_file_len(len: u64) -> ByteLen {
        ByteLen(usize::try_from(len).unwrap_or(usize::MAX))
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
    ids: TurnIdAllocator,

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

    /// The last data/index write failure, if any, cleared on the next successful
    /// persist. Persistence is best-effort (in-memory turns stay authoritative),
    /// but a caller can observe a failed write via
    /// [`TranscriptStore::last_persist_error`] instead of only a log line.
    last_persist_error: Option<FsError>,

    /// Whether a [`GroupGuard`] currently holds an open turn. Guards the
    /// single-open-turn invariant: two live guards would interleave their
    /// messages in the one `open_group` buffer. Set in [`TranscriptStore::group`],
    /// cleared when the guard commits/drops.
    turn_open: bool,
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
    transcript_id: u32,
    data_path: String,
    index_path: String,
    /// When true this store is in-memory only: it loads nothing at construction
    /// and every persist is a no-op, so it never touches the filesystem. The
    /// paths are unused (left empty) in this mode.
    volatile: bool,
    state: Mutex<StoreState>,
    _fs: PhantomData<fn() -> F>,
}

/// The agent's complete transcript: an append-only, verbatim record
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
/// # use claw_memory::TranscriptStore;
/// MemFs::new();
/// let mut store = TranscriptStore::<MemFs>::new(42, "/data/transcripts")
///     .expect("a fresh MemFs has no data log, so the transcript starts empty");
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

/// Failure building a [`TranscriptStore`] from its on-disk log.
///
/// A *missing* transcript is not an error (it starts empty); a mismatched or
/// unreadable *index* is recovered by rebuilding from the data log. This is
/// returned only when the data log itself exists but cannot be read, so a real
/// I/O failure is never silently mistaken for an empty transcript (which would
/// then be overwritten on the next turn).
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum TranscriptInitError {
    /// The transcript data log exists but could not be read.
    #[error("transcript data log {path} is unreadable: {source}")]
    Unreadable {
        /// The data-log path that failed to load.
        path: String,
        /// The underlying filesystem error.
        #[source]
        source: FsError,
    },
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
    /// Build the store for `transcript_id`, restoring its persisted contents if
    /// present (missing or unreadable files start empty).
    ///
    /// Different ids map to different files under `dir`, so each transcript is
    /// stored independently. A mismatched or unreadable index is rebuilt from the
    /// data log during construction.
    ///
    /// # Examples
    ///
    /// ```
    /// # use claw_interface::MemFs;
    /// # use claw_memory::TranscriptStore;
    /// MemFs::new();
    /// let store = TranscriptStore::<MemFs>::new(7, "/data/transcripts")
    ///     .expect("a fresh MemFs has no data log, so the transcript starts empty");
    /// assert_eq!(store.transcript_id(), 7);
    /// assert!(store.messages().as_array().unwrap().is_empty()); // missing files start empty
    /// ```
    ///
    /// # Errors
    ///
    /// [`TranscriptInitError::Unreadable`] when the transcript *data log*
    /// exists but cannot be read. A missing transcript starts empty, and a
    /// corrupt/mismatched *index* is transparently rebuilt from the data log.
    pub fn new(transcript_id: u32, dir: &str) -> Result<Self, TranscriptInitError> {
        let data_path = transcript_path(dir, transcript_id, DATA_EXT);
        let index_path = transcript_path(dir, transcript_id, INDEX_EXT);
        let (mut state, needs_rebuild) =
            load_state::<F>(&data_path, &index_path).map_err(|source| {
                TranscriptInitError::Unreadable {
                    path: data_path.clone(),
                    source,
                }
            })?;
        if needs_rebuild {
            write_live_set_to_files::<F>(&data_path, &index_path, &mut state, transcript_id);
        }
        Ok(Self {
            inner: Arc::new(StoreInner {
                transcript_id,
                data_path,
                index_path,
                volatile: false,
                state: Mutex::new(state),
                _fs: PhantomData,
            }),
        })
    }

    /// Build an **in-memory** store for `transcript_id`: it starts empty, never
    /// reads or writes the filesystem, and every persist is a no-op.
    ///
    /// Used for transcripts that are pure context-management scratch and need not
    /// survive a restart (subagents), so no `dir` is required. The write/read API
    /// is otherwise identical to a persisting store.
    pub fn in_memory(transcript_id: u32) -> Self {
        Self {
            inner: Arc::new(StoreInner {
                transcript_id,
                data_path: String::new(),
                index_path: String::new(),
                volatile: true,
                state: Mutex::new(StoreState::default()),
                _fs: PhantomData,
            }),
        }
    }

    /// This store's transcript id.
    pub fn transcript_id(&self) -> u32 {
        self.inner.transcript_id
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
    ///
    /// Only one turn may be open at a time: a second overlapping `group()` would
    /// interleave its messages into the single `open_group` buffer. That is a
    /// caller bug (turns are opened and driven sequentially), caught by a
    /// `debug_assert!` and logged in release rather than silently corrupting the
    /// turn.
    pub fn group(&self) -> GroupGuard<F> {
        {
            let mut state = self.lock_state();
            debug_assert!(
                !state.turn_open,
                "transcript {}: group() called while a turn is already open",
                self.inner.transcript_id
            );
            if state.turn_open {
                log::warn!(
                    "transcript {}: group() called while a turn is already open; \
                     messages will interleave",
                    self.inner.transcript_id
                );
            }
            state.turn_open = true;
        }
        GroupGuard {
            inner: Arc::clone(&self.inner),
        }
    }

    /// The last persistence failure, if any.
    ///
    /// Data/index writes are best-effort: a failure leaves the in-memory turns
    /// authoritative and logs, but the write did not land. This exposes that
    /// failure so a caller can react (retry [`flush`](Self::flush), warn) instead
    /// of discovering the loss only on the next reboot. Cleared on the next
    /// successful persist.
    pub fn last_persist_error(&self) -> Option<FsError> {
        self.lock_state().last_persist_error.clone()
    }

    /// Append a user message to the current open turn.
    ///
    /// This is the direct writer form for callers that already own turn lifetime.
    /// Call [`commit_open_turn`](Self::commit_open_turn) when the turn boundary is
    /// reached. For RAII grouping, prefer [`group`](Self::group).
    pub fn push_user_message(&self, content: impl Into<String>) {
        push_open(
            &self.inner,
            json!({ "role": "user", "content": content.into() }),
        );
    }

    /// Append a raw assistant message object to the current open turn.
    ///
    /// `raw_message_json` is the backend-shaped assistant message object; an
    /// unparseable value is logged and dropped rather than corrupting the turn.
    pub fn push_assistant_message(&self, raw_message_json: &str) {
        push_assistant_message(&self.inner, raw_message_json);
    }

    /// Append a tool result to the current open turn.
    ///
    /// Set `is_error` when the tool failed, so the model can see the call did not
    /// succeed.
    pub fn push_tool_result(&self, tool_call_id: &str, content: &str, is_error: bool) {
        push_open(
            &self.inner,
            json!({
                "role": "tool",
                "tool_call_id": tool_call_id,
                "content": content,
                "is_error": is_error,
            }),
        );
    }

    /// Append a whole batch of messages to the current open turn.
    ///
    /// `messages` must be a JSON array; a non-array value is logged and ignored.
    pub fn push_patch(&self, messages: &Value) {
        push_patch(&self.inner, messages);
    }

    /// Commit the current open turn as one group record, then persist if due.
    ///
    /// No-ops when the open turn is empty.
    pub fn commit_open_turn(&self) {
        commit_open_turn(&self.inner);
    }

    /// Discard the current open turn without committing or persisting it.
    ///
    /// No-ops when the open turn is empty. This is for hard cancellation paths
    /// where partially materialized user/assistant/tool messages must not become
    /// part of transcript history.
    pub fn discard_open_turn(&self) {
        discard_open_turn(&self.inner);
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
/// # use claw_memory::TranscriptStore;
/// # use serde_json::json;
/// # MemFs::new();
/// # let store = TranscriptStore::<MemFs>::new(1, "/data/transcripts").unwrap();
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
        push_open(
            &self.inner,
            json!({ "role": "user", "content": content.into() }),
        );
    }

    /// Append a raw assistant message (plain text and/or `tool_calls`).
    ///
    /// `raw_message_json` is the backend-shaped assistant message object; an
    /// unparseable value is logged and dropped rather than corrupting the turn.
    pub fn append_assistant(&self, raw_message_json: &str) {
        push_assistant_message(&self.inner, raw_message_json);
    }

    /// Append a tool result for the call `tool_call_id` to the open turn.
    ///
    /// Set `is_error` when the tool failed, so the model can see the call did not
    /// succeed.
    pub fn append_tool_result(&self, tool_call_id: &str, content: &str, is_error: bool) {
        push_open(
            &self.inner,
            json!({
                "role": "tool",
                "tool_call_id": tool_call_id,
                "content": content,
                "is_error": is_error,
            }),
        );
    }

    /// Append a whole batch of messages to the open turn (e.g. one tool round).
    ///
    /// `messages` must be a JSON array; a non-array value is logged and ignored.
    pub fn append_patch(&self, messages: &Value) {
        push_patch(&self.inner, messages);
    }

    /// Commit the open turn now as one group record, then persist if due.
    pub fn commit(&self) {
        commit_open_turn(&self.inner);
    }
}

impl<F: ClawFs + 'static> Drop for GroupGuard<F> {
    fn drop(&mut self) {
        self.commit();
        // Release the single-open-turn claim so the next `group()` is valid.
        lock_state(&self.inner).turn_open = false;
    }
}

fn push_assistant_message<F: ClawFs + 'static>(inner: &StoreInner<F>, raw_message_json: &str) {
    match serde_json::from_str::<Value>(raw_message_json) {
        Ok(message) => push_open(inner, message),
        Err(err) => log::warn!(
            "transcript {}: invalid assistant json: {err}",
            inner.transcript_id
        ),
    }
}

fn push_patch<F: ClawFs + 'static>(inner: &StoreInner<F>, messages: &Value) {
    let Some(items) = messages.as_array() else {
        log::warn!(
            "transcript {}: append_patch expected a JSON array",
            inner.transcript_id
        );
        return;
    };
    for message in items {
        push_open(inner, message.clone());
    }
}

fn push_open<F: ClawFs + 'static>(inner: &StoreInner<F>, message: Value) {
    let mut state = lock_state(inner);
    state.open_group.push(message);
    state.mark_changed();
}

fn commit_open_turn<F: ClawFs + 'static>(inner: &StoreInner<F>) {
    let due = {
        let mut state = lock_state(inner);
        if state.open_group.is_empty() {
            return;
        }
        let msgs = std::mem::take(&mut state.open_group);
        let id = state.ids.next();
        // An in-memory store never flushes, so skip enqueuing a pending line that
        // would otherwise accumulate unbounded.
        if !inner.volatile {
            enqueue(&mut state, id, msgs.clone(), inner.transcript_id);
        }
        state.groups.push(StoredGroup {
            id,
            msgs,
            loc: None,
        });
        // A committed turn changes the turn-structured snapshot (a new turn
        // appears), so invalidate caches and bump the version.
        state.mark_changed();
        persist_due(&state)
    };
    if due {
        persist(inner, false);
    }
}

fn discard_open_turn<F: ClawFs + 'static>(inner: &StoreInner<F>) {
    let mut state = lock_state(inner);
    if state.open_group.is_empty() {
        return;
    }
    state.open_group.clear();
    state.mark_changed();
}

/// Serialize a group record to a data line and queue it for the next append.
fn enqueue(state: &mut StoreState, id: TurnId, msgs: Vec<Value>, transcript_id: u32) {
    let record = LogRecord::Group { id, msgs };
    match serde_json::to_vec(&record) {
        Ok(mut line) => {
            line.push(b'\n');
            state.pending.push(Pending { line, id });
        }
        Err(err) => log::warn!("transcript {transcript_id}: serialize record failed: {err}"),
    }
}

/// Flush pending records (one `append`) and, when needed, rewrite the manifest.
fn persist<F: ClawFs + 'static>(inner: &StoreInner<F>, force_manifest: bool) {
    // An in-memory store keeps everything in `state`; it never writes files.
    if inner.volatile {
        return;
    }
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
        if let Err(err) = F::append(&inner.data_path, &data_buf) {
            log::warn!(
                "transcript {}: data append failed: {err}",
                inner.transcript_id
            );
            state.last_persist_error = Some(err);
            return;
        }
        state.data_len = off.as_len();
        for (id, off, len) in locs {
            set_loc(&mut state, id, off, len);
        }
        state.pending.clear();
    }
    state.last_persist = Some(Instant::now());

    if let Some(bytes) = build_manifest_bytes(&state, inner.transcript_id) {
        match F::write_atomic(&inner.index_path, &bytes) {
            Ok(()) => {
                state.manifest_covered_len = state.data_len;
                state.last_persist_error = None;
            }
            Err(err) => {
                log::warn!(
                    "transcript {}: index write failed: {err}",
                    inner.transcript_id
                );
                state.last_persist_error = Some(err);
            }
        }
    }
}

/// Rewrite `.jsonl` + `.json` from the in-memory turns in id order, updating
/// state locs to the new layout on success.
fn write_live_set_to_files<F: ClawFs>(
    data_path: &str,
    index_path: &str,
    state: &mut StoreState,
    transcript_id: u32,
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
        let Some(len) = append_line(&mut data_buf, &record, transcript_id) else {
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
        next_id: state.ids.peek(),
        live,
    };
    let manifest_bytes = match serde_json::to_vec(&manifest) {
        Ok(bytes) => bytes,
        Err(err) => {
            log::warn!(
                "transcript {transcript_id}: write_live manifest serialize failed: {err}"
            );
            return;
        }
    };

    if let Err(err) = F::write_atomic(data_path, &data_buf) {
        log::warn!("transcript {transcript_id}: write_live data write failed: {err}");
        state.last_persist_error = Some(err);
        return;
    }
    if let Err(err) = F::write_atomic(index_path, &manifest_bytes) {
        log::warn!("transcript {transcript_id}: write_live index write failed: {err}");
        state.last_persist_error = Some(err);
        // Data file is the fresh truth; stale manifest is rebuilt on next load.
    } else {
        state.last_persist_error = None;
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
fn append_line(buf: &mut Vec<u8>, record: &LogRecord, transcript_id: u32) -> Option<ByteLen> {
    match serde_json::to_vec(record) {
        Ok(mut line) => {
            line.push(b'\n');
            let len = ByteLen::of(&line);
            buf.extend_from_slice(&line);
            Some(len)
        }
        Err(err) => {
            log::warn!("transcript {transcript_id}: serialize record failed: {err}");
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
fn build_manifest_bytes(state: &StoreState, transcript_id: u32) -> Option<Vec<u8>> {
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
        next_id: state.ids.peek(),
        live,
    };
    match serde_json::to_vec(&manifest) {
        Ok(bytes) => Some(bytes),
        Err(err) => {
            log::warn!("transcript {transcript_id}: manifest serialize failed: {err}");
            None
        }
    }
}

fn persist_due(state: &StoreState) -> bool {
    if state.pending.is_empty() {
        return false;
    }
    match state.last_persist {
        None => true,
        Some(at) => at.elapsed() >= DEFAULT_PERSIST_DEBOUNCE,
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
///
/// # Errors
///
/// [`FsError`] when the data log exists but cannot be opened. A *missing* data
/// log ([`FsError::NotFound`]) yields an empty state, and index problems are
/// recovered from the data log rather than surfaced — so a genuine data-log I/O
/// fault is never silently treated as an empty transcript.
fn load_state<F: ClawFs>(data_path: &str, index_path: &str) -> Result<(StoreState, bool), FsError> {
    let mut state = StoreState::default();
    let mut covered_len = ByteLen::default();
    let mut manifest_next_id = TurnId::default();
    let mut mismatch = false;

    // One handle to the data log, reused for every indexed record read and the
    // tail scan below, instead of reopening the file per access. A missing log is
    // a fresh transcript; any other open failure is a real fault, surfaced so
    // the empty state is not mistaken for "no transcript".
    let mut data_file = match F::open(data_path) {
        Ok(file) => Some(file),
        Err(FsError::NotFound) => None,
        Err(error) => return Err(error),
    };

    match F::read(index_path) {
        Ok(bytes) => {
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
                                    "transcript load: manifest entry at offset {} does not \
                                     match data log record; rebuilding",
                                    off.as_u64()
                                );
                                mismatch = true;
                                break 'entries;
                            }
                            None => {
                                log::error!(
                                    "transcript load: manifest entry at offset {} could not \
                                     be parsed; rebuilding",
                                    off.as_u64()
                                );
                                mismatch = true;
                                break 'entries;
                            }
                        },
                        Err(err) => {
                            log::error!(
                                "transcript load: manifest entry at offset {} could not be \
                                 read: {err}; rebuilding",
                                off.as_u64()
                            );
                            mismatch = true;
                            break 'entries;
                        }
                    }
                }
            } else if data_file.is_some() {
                log::error!("transcript load: manifest could not be parsed; rebuilding");
                mismatch = true;
            }
        }
        Err(FsError::NotFound) => {
            if data_file.is_some() {
                mismatch = true;
            }
        }
        Err(err) => {
            log::error!("transcript load: manifest could not be read: {err}; rebuilding");
            if data_file.is_some() {
                mismatch = true;
            }
        }
    }

    if mismatch {
        state = StoreState::default();
        covered_len = ByteLen::default();
        manifest_next_id = TurnId::default();
    }

    let mut data_file_len = 0;
    if let Some(file) = data_file.as_ref() {
        if let Ok(len) = file.size() {
            data_file_len = len;
        }
    }
    let data_len = ByteLen::from_file_len(data_file_len);
    if data_len > covered_len {
        let extra = data_len.saturating_sub(covered_len);
        if let Some(file) = data_file.as_mut() {
            let tail = file.read_exact_at(covered_len.as_offset().as_u64(), extra.as_usize())?;
            scan_tail(&mut state, &tail, covered_len.as_offset());
        }
    }

    state.data_len = data_len;
    state.manifest_covered_len = covered_len;
    state.groups.sort_by_key(|g| g.id);
    state.ids = TurnIdAllocator::starting_at(manifest_next_id.max(max_seen_id(&state).next()));
    Ok((state, mismatch))
}

/// Parse a newline-delimited tail buffer, applying each complete record.
fn scan_tail(state: &mut StoreState, tail: &[u8], base_off: ByteOffset) {
    let mut start = 0usize;
    let mut pos = base_off;
    for (i, byte) in tail.iter().enumerate() {
        if *byte != b'\n' {
            continue;
        }
        let line = &tail[start..i];
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
    let mut max = TurnId::default();
    for group in &state.groups {
        if group.id > max {
            max = group.id;
        }
    }
    max
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
            log::warn!("transcript load: skipping unparseable record: {err}");
            None
        }
    }
}

/// Build a per-transcript path from the base dir, id, and extension.
fn transcript_path(dir: &str, transcript_id: u32, ext: &str) -> String {
    format!("{}/{transcript_id}{ext}", dir.trim_end_matches('/'))
}

fn lock_state<F: ClawFs + 'static>(inner: &StoreInner<F>) -> MutexGuard<'_, StoreState> {
    inner
        .state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use claw_interface::MemFs;

    #[test]
    fn invalid_index_is_rebuilt_from_data_log() {
        MemFs::new();
        let dir = "/transcript-index-rebuild";
        let index_path = transcript_path(dir, 1, INDEX_EXT);

        let store = TranscriptStore::<MemFs>::new(1, dir).unwrap();
        {
            let turn = store.group();
            turn.append_user("persisted user");
            turn.append_assistant(r#"{"role":"assistant","content":"persisted reply"}"#);
        }
        store.flush();
        assert!(serde_json::from_slice::<Manifest>(&MemFs::read(&index_path).unwrap()).is_ok());

        MemFs::write_atomic(&index_path, b"{not valid json").unwrap();
        let rebuilt = TranscriptStore::<MemFs>::new(1, dir).unwrap();
        let messages = rebuilt.messages().to_string();
        assert!(messages.contains("persisted user"));
        assert!(messages.contains("persisted reply"));
        assert!(serde_json::from_slice::<Manifest>(&MemFs::read(&index_path).unwrap()).is_ok());
    }
}
