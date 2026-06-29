//! `ConversationMemory` — the agent's short-term conversation memory.
//!
//! This is the running transcript the model needs for short-term continuity. Its
//! natural unit is a **group**: one turn's worth of messages, produced together
//! by the agent loop:
//!
//! ```text
//! group {                       // one turn
//!   user message
//!   assistant message (text and/or tool_calls)
//!   tool result(s)
//!   assistant message
//! }
//! ```
//!
//! The memory owns, oldest-to-newest when rendered:
//! - zero or more **compact segments** — each covers a non-overlapping id range
//!   and carries the summary produced by the [`Compactor`] for that range.
//! - `groups`  — committed turns kept verbatim (the recent tail).
//! - the **open group** — the in-progress turn (see [`GroupGuard`]).
//!
//! Every group is stamped with a monotonic `id` so its chronological position
//! is stable across compaction and reloads.
//!
//! # Turns are built through a guard
//!
//! Conversation content is appended through a [`GroupGuard`] from
//! [`group`](ConversationMemory::group). The guard buffers the open turn and
//! commits it as a single record when it drops, so a turn can never be left
//! half-open and compaction never splits a tool round (it cuts only on group
//! boundaries).
//!
//! # Multi-level compaction
//!
//! When the total token estimate exceeds
//! [`compact_threshold_tokens`](ConversationConfig::compact_threshold_tokens), a
//! background job is scheduled. The job:
//!
//! 1. Determines the **verbatim tail** by walking backwards through the groups,
//!    accumulating tokens until
//!    [`keep_recent_tokens`](ConversationConfig::keep_recent_tokens) is reached
//!    (at least one group is always kept verbatim).
//! 2. Finds the **cursor** — the highest id already covered by a compact segment.
//! 3. Selects aged groups with ids past the cursor (so the chunk always starts at
//!    `cursor + 1`), packing them into a chunk of up to
//!    [`segment_token_budget`](ConversationConfig::segment_token_budget) tokens.
//! 4. Runs the [`Compactor`] on that chunk and parks the result.
//!
//! The foreground applies the parked result at the next turn boundary: a new
//! `compact` record is appended to the data log and the covered groups are
//! retired. Multiple compact segments accumulate as independent, non-overlapping
//! summaries; [`messages`](ConversationMemory::messages) interleaves them with
//! the verbatim tail in chronological order.
//!
//! **Coverage continuity:** because each chunk starts at `cursor + 1`, the
//! compact segments stay contiguous and abut the verbatim groups — the live set
//! always covers the entire id range with no gaps and no overlaps. Compaction
//! never drops a segment (that would strand the ids it covered); bounding the
//! growth of the summaries themselves is a future job for re-compacting compacts
//! ("leveling"), not for deleting coverage.
//!
//! # Threading: one writer, the pool only computes
//!
//! All state mutation (appends, commits, persistence, applying a compact)
//! happens on the **foreground** thread that owns the memory. Pool workers only
//! read a snapshot and park a result. The state lock is never held across the
//! [`Compactor`] call. A single memory must therefore be driven from one thread.
//!
//! # Identity and persistence
//!
//! Each memory is keyed by a `conversation_id`. Two files under
//! [`ConversationConfig::dir`]:
//!
//! - `conversation-<id>.jsonl` — the **data log**: one JSON record per line
//!   (`group` / `compact`), **append-only**. The source of truth.
//! - `conversation-<id>.json` — the **index manifest**: a rebuildable cache
//!   listing the byte `(off, len)` of every live record plus `covered_len` and
//!   `next_id`, rewritten atomically when the live/dead structure changes.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use claw_interface::{ClawFile, ClawFs};

use crate::compaction::Compactor;
use claw_utils::SharedTaskPool;

/// Token budget past which a background compaction is scheduled.
const DEFAULT_COMPACT_THRESHOLD_TOKENS: usize = 6000;
/// Token budget for the verbatim tail; groups are accumulated newest-first until
/// this is met. At least one group is always kept verbatim.
const DEFAULT_KEEP_RECENT_TOKENS: usize = 2000;
/// Max tokens fed to the compactor per chunk (one background job per chunk).
const DEFAULT_SEGMENT_TOKEN_BUDGET: usize = 1500;
/// Minimum gap between persistence writes (flash-wear debounce).
const DEFAULT_PERSIST_DEBOUNCE: Duration = Duration::from_secs(5);
/// Rough bytes-per-token divisor for the size estimate. See [`estimate_message_tokens`].
const CHARS_PER_TOKEN: usize = 4;
/// Per-conversation filenames: `{dir}/{FILE_PREFIX}{id}{DATA_EXT|INDEX_EXT}`.
const FILE_PREFIX: &str = "conversation-";
const DATA_EXT: &str = ".jsonl";
const INDEX_EXT: &str = ".json";
/// Manifest schema version, so a future layout change can be detected on load.
const MANIFEST_VERSION: u32 = 1;
/// Don't collapse below this data size — tiny files aren't worth rewriting.
const COLLAPSE_FLOOR_BYTES: ByteLen = ByteLen(8 * 1024);

/// A monotonic logical identifier for a group.
///
/// This fixes chronological order and is the *only* thing compaction supersedes
/// (see [`apply_record`]). It is deliberately a distinct type from byte
/// offsets/lengths so the two can never be swapped: supersession is keyed on
/// `RecordId`, addressing on [`ByteOffset`].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(transparent)]
struct RecordId(u64);

impl RecordId {
    /// The id that immediately follows this one.
    fn next(self) -> RecordId {
        RecordId(self.0.saturating_add(1))
    }
}

/// A byte position within the data log. Addressing only — never compared for
/// chronology (that is [`RecordId`]'s job).
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
    fn saturating_add(self, other: ByteLen) -> ByteLen {
        ByteLen(self.0.saturating_add(other.0))
    }
    fn saturating_sub(self, other: ByteLen) -> ByteLen {
        ByteLen(self.0.saturating_sub(other.0))
    }
    fn saturating_mul(self, factor: usize) -> ByteLen {
        ByteLen(self.0.saturating_mul(factor))
    }
    /// From a [`ClawFs::len`] result, clamping if it somehow exceeds `usize` (a
    /// 32-bit device can only address `usize` bytes; conversation files are tiny).
    fn from_file_len(len: u64) -> ByteLen {
        ByteLen(usize::try_from(len).unwrap_or(usize::MAX))
    }
}

/// Tuning for a [`ConversationMemory`].
///
/// Construct with [`ConversationConfig::new`] for sensible defaults, then
/// override individual fields as needed.
///
/// # Examples
///
/// ```
/// use std::time::Duration;
///
/// use claw_memory::ConversationConfig;
///
/// // Defaults, then tighten the compaction trigger and disable the write debounce.
/// let config = ConversationConfig {
///     compact_threshold_tokens: 4_000,
///     persist_debounce: Duration::ZERO,
///     ..ConversationConfig::new("/data/conversations")
/// };
///
/// assert_eq!(config.dir, "/data/conversations");
/// assert_eq!(config.compact_threshold_tokens, 4_000);
/// ```
pub struct ConversationConfig {
    /// Base directory for per-conversation files, already resolved against the
    /// DATA root by the caller. The filenames are derived from the conversation id.
    pub dir: String,
    /// When the estimate exceeds this, compaction is scheduled in the background.
    pub compact_threshold_tokens: usize,
    /// Token budget for the verbatim tail. Groups are kept newest-first until
    /// their cumulative tokens meet this budget; at least one group is always
    /// kept regardless.
    pub keep_recent_tokens: usize,
    /// Max tokens consumed from aged groups per compaction chunk. Controls how
    /// much history is summarised in one background job.
    pub segment_token_budget: usize,
    /// Minimum interval between filesystem writes.
    pub persist_debounce: Duration,
}

impl ConversationConfig {
    /// Config for conversation files under `dir`, with default tuning otherwise.
    ///
    /// `dir` is the base directory for the per-conversation files; the filenames
    /// themselves are derived from the conversation id. Accepts anything
    /// `Into<String>` (`&str`, `String`, …).
    ///
    /// # Examples
    ///
    /// ```
    /// use claw_memory::ConversationConfig;
    ///
    /// let config = ConversationConfig::new("/data/conversations");
    /// assert_eq!(config.dir, "/data/conversations");
    /// ```
    pub fn new(dir: impl Into<String>) -> Self {
        Self {
            dir: dir.into(),
            compact_threshold_tokens: DEFAULT_COMPACT_THRESHOLD_TOKENS,
            keep_recent_tokens: DEFAULT_KEEP_RECENT_TOKENS,
            segment_token_budget: DEFAULT_SEGMENT_TOKEN_BUDGET,
            persist_debounce: DEFAULT_PERSIST_DEBOUNCE,
        }
    }
}

/// Injected collaborators for a [`ConversationMemory`].
///
/// The persistence backend `F` is a concrete, statically dispatched [`ClawFs`]
/// (held by value, not behind `Arc<dyn>`): the device passes its single on-disk
/// implementation, host CLIs and tests pass `MemFs`/`DiskFs`. Calls compile down
/// to direct (monomorphized) calls with no vtable. The summarization seam stays
/// dynamic (`Arc<dyn Compactor>`): it is swapped per build and not a hot path.
pub struct ConversationDeps<F: ClawFs + 'static> {
    /// Persistence backend (read on construction, written on the foreground).
    pub fs: F,
    /// Shared worker pool that runs the summarization compute off the tick path.
    pub pool: Arc<SharedTaskPool>,
    /// How aged windows are summarized.
    pub compactor: Arc<dyn Compactor>,
}

/// One record as stored on a line of the data `.jsonl`.
#[derive(Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
enum LogRecord {
    /// A committed turn: its messages in order.
    Group { id: RecordId, msgs: Vec<Value> },
    /// A summary that supersedes groups in `[id_start, id_end]`.
    Compact {
        id_start: RecordId,
        id_end: RecordId,
        summary: Vec<Value>,
    },
}

/// One live record's location inside the data `.jsonl`, as stored in the manifest.
#[derive(Clone, Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
enum IndexEntry {
    Group {
        off: ByteOffset,
        len: ByteLen,
        id: RecordId,
    },
    Compact {
        off: ByteOffset,
        len: ByteLen,
        id_start: RecordId,
        id_end: RecordId,
    },
}

/// The index manifest: the live layout of the data log, rewritten atomically.
#[derive(Default, Serialize, Deserialize)]
struct Manifest {
    version: u32,
    /// Data-log byte length this manifest describes; load tail-scans past it.
    covered_len: ByteLen,
    next_id: RecordId,
    live: Vec<IndexEntry>,
}

/// A committed turn plus its byte location in the data log (once flushed).
struct StoredGroup {
    id: RecordId,
    msgs: Vec<Value>,
    loc: Option<(ByteOffset, ByteLen)>,
}

/// A live compact segment: summary messages covering groups in `[id_start, id_end]`.
struct StoredCompact {
    id_start: RecordId,
    id_end: RecordId,
    messages: Vec<Value>,
    /// Byte location of this compact record in the data log, once flushed.
    loc: Option<(ByteOffset, ByteLen)>,
}

/// Which in-memory record a pending data line belongs to, so its `loc` can be
/// written back once the line is appended.
#[derive(Clone, Copy)]
enum PendingTarget {
    Group(RecordId),
    /// `id_start` of the compact segment this line belongs to.
    Compact(RecordId),
}

/// A serialized data line awaiting its append.
struct Pending {
    line: Vec<u8>,
    target: PendingTarget,
}

/// A chunk summary computed by a pool worker, parked until the foreground applies it.
struct CompactionResult {
    id_start: RecordId,
    id_end: RecordId,
    summary: Vec<Value>,
}

/// The lock-protected contents of the memory.
#[derive(Default)]
struct MemoryState {
    /// Live compact segments, sorted ascending by `id_start`.
    compacts: Vec<StoredCompact>,
    groups: Vec<StoredGroup>,
    /// The in-progress turn's messages — not yet committed (volatile, no id).
    open_group: Vec<Value>,
    next_id: RecordId,

    /// Records appended in memory but not yet written to the `.jsonl`.
    pending: Vec<Pending>,
    /// Current byte length of the `.jsonl` (also the next append offset).
    data_len: ByteLen,
    /// Data length the on-disk manifest currently describes.
    manifest_covered_len: ByteLen,
    /// The manifest must be rewritten (a compaction/collapse changed live/dead).
    index_dirty: bool,

    /// A compact result a pool worker finished but the foreground has not yet applied.
    parked_compact: Option<CompactionResult>,

    /// Cached model-ready snapshot returned by [`ConversationMemory::messages`].
    /// Built lazily on first read and shared as a cheap `Arc` clone; invalidated
    /// (set to `None`) whenever message content changes (an open-turn append or a
    /// compaction applied), so each iteration bumps a refcount instead of cloning
    /// the whole transcript.
    messages_cache: Option<Arc<Value>>,
    /// Monotonic content version, bumped in lockstep with every `messages_cache`
    /// invalidation (an open-turn append or an applied compaction). A pull-based
    /// reader caches work keyed on the transcript and recomputes only when this
    /// advances — see [`ConversationMemory::version`].
    version: u64,

    approx_tokens: usize,
    last_persist: Option<Instant>,
}

/// Shared inner state — held behind an `Arc` so the pool worker and the agent
/// reference the same memory.
struct MemoryInner<F: ClawFs + 'static> {
    conversation_id: usize,
    data_path: String,
    index_path: String,
    state: Mutex<MemoryState>,
    /// Single-flight guard: at most one compaction job in the pool at a time.
    compaction_in_flight: AtomicBool,
    config: ConversationConfig,
    deps: ConversationDeps<F>,
}

/// The agent's short-term conversation memory. See the module docs for the
/// storage layout and the compaction model.
///
/// Build one with [`new`](Self::new), append turns through the
/// [`GroupGuard`] returned by [`group`](Self::group), read the model-ready
/// transcript with [`messages`](Self::messages), and checkpoint with
/// [`flush`](Self::flush). Drive a single memory from one thread.
///
/// # Examples
///
/// ```
/// # use std::sync::Arc;
/// # use claw_interface::{MemFs, StdThread};
/// # use claw_memory::{
/// #     CompactError, Compactor, ConversationConfig, ConversationDeps,
/// #     ConversationMemory,
/// # };
/// # use claw_utils::{SharedTaskPool, PoolConfig};
/// # use serde_json::Value;
/// # struct StubCompactor;
/// # impl Compactor for StubCompactor {
/// #     fn compact(&self, _: &[Value]) -> Result<Vec<Value>, CompactError> { Ok(vec![]) }
/// # }
/// let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
/// let mut memory = ConversationMemory::new(
///     42,
///     ConversationConfig::new("/data/conversations"),
///     ConversationDeps {
///         fs: MemFs::new(),
///         pool: Arc::clone(&pool),
///         compactor: Arc::new(StubCompactor),
///     },
/// );
///
/// // One turn = one `group()`; the whole turn commits when the guard drops.
/// {
///     let turn = memory.group();
///     turn.append_user("what's the weather?");
///     turn.append_assistant(r#"{"role":"assistant","content":"Sunny."}"#);
/// }
///
/// let rendered = memory.messages();
/// assert_eq!(rendered.as_array().map(|m| m.len()), Some(2));
/// memory.flush(); // checkpoint, e.g. on a clean shutdown
/// # Ok::<(), std::io::Error>(())
/// ```
pub struct ConversationMemory<F: ClawFs + 'static> {
    inner: Arc<MemoryInner<F>>,
}

// Manual `Clone`: only the `Arc` is cloned, so this is cheap and does **not**
// require `F: Clone` (a `#[derive(Clone)]` would wrongly add that bound).
impl<F: ClawFs + 'static> Clone for ConversationMemory<F> {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl<F: ClawFs + 'static> ConversationMemory<F> {
    /// Build the memory for `conversation_id`, restoring its persisted contents
    /// if present (missing or unreadable files start empty).
    ///
    /// Different ids map to different files under [`ConversationConfig::dir`], so
    /// each conversation is stored independently. A mismatched or unreadable
    /// index is rebuilt from the data log during construction.
    ///
    /// # Examples
    ///
    /// ```
    /// # use std::sync::Arc;
    /// # use claw_interface::{MemFs, StdThread};
    /// # use claw_memory::{
    /// #     CompactError, Compactor, ConversationConfig, ConversationDeps,
    /// #     ConversationMemory,
    /// # };
    /// # use claw_utils::{SharedTaskPool, PoolConfig};
    /// # use serde_json::Value;
    /// # struct StubCompactor;
    /// # impl Compactor for StubCompactor {
    /// #     fn compact(&self, _: &[Value]) -> Result<Vec<Value>, CompactError> { Ok(vec![]) }
    /// # }
    /// # let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
    /// let memory = ConversationMemory::new(
    ///     7,
    ///     ConversationConfig::new("/data/conversations"),
    ///     ConversationDeps { fs: MemFs::new(), pool, compactor: Arc::new(StubCompactor) },
    /// );
    /// assert_eq!(memory.conversation_id(), 7);
    /// assert!(memory.messages().as_array().unwrap().is_empty()); // missing files start empty
    /// # Ok::<(), std::io::Error>(())
    /// ```
    pub fn new(
        conversation_id: usize,
        config: ConversationConfig,
        deps: ConversationDeps<F>,
    ) -> Self {
        let data_path = conversation_path(&config.dir, conversation_id, DATA_EXT);
        let index_path = conversation_path(&config.dir, conversation_id, INDEX_EXT);
        let (mut state, needs_rebuild) = load_state(&deps.fs, &data_path, &index_path);
        if needs_rebuild {
            write_live_set_to_files(
                &deps.fs,
                &data_path,
                &index_path,
                &mut state,
                conversation_id,
            );
        }
        Self {
            inner: Arc::new(MemoryInner {
                conversation_id,
                data_path,
                index_path,
                state: Mutex::new(state),
                compaction_in_flight: AtomicBool::new(false),
                config,
                deps,
            }),
        }
    }

    /// This memory's conversation id.
    pub fn conversation_id(&self) -> usize {
        self.inner.conversation_id
    }

    /// A monotonic counter bumped whenever the rendered transcript changes (an
    /// open-turn append or an applied compaction).
    ///
    /// Lets a pull-based reader cache output keyed on the transcript and rebuild
    /// only when this advances, without diffing [`messages`](Self::messages). It
    /// is a content version, not a structural one: committing an open turn moves
    /// messages between buffers but leaves the rendered output (and this counter)
    /// unchanged.
    pub fn version(&self) -> u64 {
        self.lock_state().version
    }

    /// Open a turn. Append its messages through the returned [`GroupGuard`]; the
    /// whole turn is committed as one group when the guard drops.
    ///
    /// Takes `&mut self`, so the borrow checker allows only one open turn at a
    /// time. The guard derefs to `&ConversationMemory`, so
    /// [`messages`](Self::messages) is reachable through it while the turn is
    /// open (and includes the in-progress turn).
    ///
    /// # Examples
    ///
    /// ```
    /// # use std::sync::Arc;
    /// # use claw_interface::{MemFs, StdThread};
    /// # use claw_memory::{
    /// #     CompactError, Compactor, ConversationConfig, ConversationDeps,
    /// #     ConversationMemory,
    /// # };
    /// # use claw_utils::{SharedTaskPool, PoolConfig};
    /// # use serde_json::Value;
    /// # struct StubCompactor;
    /// # impl Compactor for StubCompactor {
    /// #     fn compact(&self, _: &[Value]) -> Result<Vec<Value>, CompactError> { Ok(vec![]) }
    /// # }
    /// # let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
    /// # let mut memory = ConversationMemory::new(
    /// #     1,
    /// #     ConversationConfig::new("/data/conversations"),
    /// #     ConversationDeps { fs: MemFs::new(), pool, compactor: Arc::new(StubCompactor) },
    /// # );
    /// {
    ///     let turn = memory.group();
    ///     turn.append_user("hi");
    ///     turn.append_assistant(r#"{"role":"assistant","content":"hello"}"#);
    ///     turn.append_tool_result("call_1", "{\"ok\":true}", false);
    /// } // drop → the 3 messages commit as one group
    ///
    /// assert_eq!(memory.messages().as_array().map(|m| m.len()), Some(3));
    /// # Ok::<(), std::io::Error>(())
    /// ```
    pub fn group(&self) -> GroupGuard<F> {
        GroupGuard {
            inner: Arc::clone(&self.inner),
        }
    }

    /// The current messages, ready to send to the model in chronological order:
    /// compact-segment messages interleaved with verbatim groups by id, followed
    /// by the in-progress open turn. Returns a shared, internally consistent
    /// JSON array snapshot; compaction state is already folded in.
    ///
    /// Compact segments and groups are non-overlapping by construction, so the
    /// merge is a simple ordered walk keyed on each segment's starting id.
    ///
    /// # Examples
    ///
    /// ```
    /// # use std::sync::Arc;
    /// # use claw_interface::{MemFs, StdThread};
    /// # use claw_memory::{
    /// #     CompactError, Compactor, ConversationConfig, ConversationDeps,
    /// #     ConversationMemory,
    /// # };
    /// # use claw_utils::{SharedTaskPool, PoolConfig};
    /// # use serde_json::Value;
    /// # struct StubCompactor;
    /// # impl Compactor for StubCompactor {
    /// #     fn compact(&self, _: &[Value]) -> Result<Vec<Value>, CompactError> { Ok(vec![]) }
    /// # }
    /// # let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
    /// # let mut memory = ConversationMemory::new(
    /// #     1,
    /// #     ConversationConfig::new("/data/conversations"),
    /// #     ConversationDeps { fs: MemFs::new(), pool, compactor: Arc::new(StubCompactor) },
    /// # );
    /// memory.group().append_user("first");
    /// memory.group().append_user("second");
    ///
    /// let rendered = memory.messages();
    /// let items = rendered.as_array().unwrap();
    /// assert_eq!(items[0]["content"], "first"); // oldest first
    /// assert_eq!(items[1]["content"], "second");
    /// # Ok::<(), std::io::Error>(())
    /// ```
    ///
    /// The snapshot is cached and shared as an `Arc`: repeated calls between
    /// mutations return a cheap refcount bump rather than rebuilding/cloning the
    /// transcript. Holding the returned `Arc` keeps that snapshot alive even if
    /// the memory mutates afterwards (the next read rebuilds a fresh one).
    pub fn messages(&self) -> Arc<Value> {
        let mut state = self.lock_state();
        if let Some(cached) = &state.messages_cache {
            return Arc::clone(cached);
        }
        let mut out = Vec::new();

        let mut compact_idx = 0usize;
        let mut group_idx = 0usize;

        loop {
            let next_compact = state.compacts.get(compact_idx);
            let next_group = state.groups.get(group_idx);
            match (next_compact, next_group) {
                (None, None) => break,
                (Some(compact), None) => {
                    out.extend(compact.messages.iter().cloned());
                    compact_idx = compact_idx.saturating_add(1);
                }
                (None, Some(group)) => {
                    out.extend(group.msgs.iter().cloned());
                    group_idx = group_idx.saturating_add(1);
                }
                (Some(compact), Some(group)) => {
                    if compact.id_start < group.id {
                        out.extend(compact.messages.iter().cloned());
                        compact_idx = compact_idx.saturating_add(1);
                    } else {
                        out.extend(group.msgs.iter().cloned());
                        group_idx = group_idx.saturating_add(1);
                    }
                }
            }
        }

        out.extend(state.open_group.iter().cloned());
        let snapshot = Arc::new(Value::Array(out));
        state.messages_cache = Some(Arc::clone(&snapshot));
        snapshot
    }

    /// Apply any parked compact, force pending changes to disk now (ignoring the
    /// debounce), refresh the index manifest, and reclaim dead space.
    ///
    /// Persists only **committed** turns; an open turn is committed when its
    /// [`GroupGuard`] drops. The clean-shutdown order is therefore "drop the
    /// guard, then `flush`". This is the manual, immediate form of the automatic
    /// debounced persistence — same writes, just now.
    ///
    /// # Examples
    ///
    /// ```
    /// # use std::sync::Arc;
    /// # use claw_interface::{MemFs, StdThread};
    /// # use claw_memory::{
    /// #     CompactError, Compactor, ConversationConfig, ConversationDeps,
    /// #     ConversationMemory,
    /// # };
    /// # use claw_utils::{SharedTaskPool, PoolConfig};
    /// # use serde_json::Value;
    /// # struct StubCompactor;
    /// # impl Compactor for StubCompactor {
    /// #     fn compact(&self, _: &[Value]) -> Result<Vec<Value>, CompactError> { Ok(vec![]) }
    /// # }
    /// # let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
    /// # let mut memory = ConversationMemory::new(
    /// #     1,
    /// #     ConversationConfig::new("/data/conversations"),
    /// #     ConversationDeps { fs: MemFs::new(), pool, compactor: Arc::new(StubCompactor) },
    /// # );
    /// memory.group().append_user("remember this");
    /// memory.flush(); // committed turn is now on disk
    /// # Ok::<(), std::io::Error>(())
    /// ```
    pub fn flush(&self) {
        apply_parked_compact(&self.inner);
        persist(&self.inner, true);
        maybe_collapse(&self.inner);
    }

    fn schedule_compaction(&self) {
        schedule_compaction(&self.inner);
    }

    fn lock_state(&self) -> MutexGuard<'_, MemoryState> {
        lock_state(&self.inner)
    }
}

/// An open turn. Append messages through it; the turn is committed as one group
/// record when the guard drops (or on an explicit [`commit`](Self::commit)).
///
/// Obtained from [`ConversationMemory::group`]. It holds `&mut ConversationMemory`,
/// so only one can be live at a time, and derefs to `&ConversationMemory` so
/// reads like [`messages`](ConversationMemory::messages) work while the turn is
/// open.
///
/// # Examples
///
/// ```
/// # use std::sync::Arc;
/// # use claw_interface::{MemFs, StdThread};
/// # use claw_memory::{
/// #     CompactError, Compactor, ConversationConfig, ConversationDeps,
/// #     ConversationMemory,
/// # };
/// # use claw_utils::{SharedTaskPool, PoolConfig};
/// # use serde_json::{json, Value};
/// # struct StubCompactor;
/// # impl Compactor for StubCompactor {
/// #     fn compact(&self, _: &[Value]) -> Result<Vec<Value>, CompactError> { Ok(vec![]) }
/// # }
/// # let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
/// # let mut memory = ConversationMemory::new(
/// #     1,
/// #     ConversationConfig::new("/data/conversations"),
/// #     ConversationDeps { fs: MemFs::new(), pool, compactor: Arc::new(StubCompactor) },
/// # );
/// let turn = memory.group();
/// turn.append_user("call the weather tool");
/// turn.append_patch(&json!([
///     { "role": "assistant", "content": "calling tool" },
///     { "role": "tool", "tool_call_id": "c1", "content": "{\"temp_c\":21}" },
/// ]));
/// // Reads see the open turn before it commits.
/// assert_eq!(memory.messages().as_array().map(|m| m.len()), Some(3));
/// turn.commit(); // or just let it drop
/// # Ok::<(), std::io::Error>(())
/// ```
/// An open turn. Append messages through it; the turn is committed as one group
/// record when the guard drops (or on an explicit [`commit`](Self::commit)).
///
/// Obtained from [`ConversationMemory::group`]. Holds an `Arc` into the memory's
/// inner state so it carries no lifetime and can be stored across async boundaries
/// or inside structs like `BaseAgent`.  Only one group should be open at a time per
/// memory instance; behaviour is unspecified if two live `GroupGuard`s share the
/// same memory (their messages interleave in the single `open_group` buffer).
pub struct GroupGuard<F: ClawFs + 'static> {
    inner: Arc<MemoryInner<F>>,
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
    /// Set `is_error` when the tool failed, so the model can see the call did
    /// not succeed.
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

    /// Commit the open turn now as one group record.
    ///
    /// A parked compact is applied first, then the turn is committed, then we
    /// maybe schedule the next compaction chunk and persist.
    pub fn commit(&self) {
        let inner = &self.inner;
        let applied = apply_parked_compact(inner);
        let (should_compact, due) = {
            let mut state = lock_state(inner);
            if !state.open_group.is_empty() {
                let msgs = std::mem::take(&mut state.open_group);
                let id = next_id(&mut state);
                enqueue(
                    &mut state,
                    &LogRecord::Group {
                        id,
                        msgs: msgs.clone(),
                    },
                    PendingTarget::Group(id),
                    inner.conversation_id,
                );
                state.groups.push(StoredGroup {
                    id,
                    msgs,
                    loc: None,
                });
            }
            let should_compact = state.approx_tokens > inner.config.compact_threshold_tokens
                && state.parked_compact.is_none();
            (should_compact, persist_due(inner, &state))
        };
        if should_compact {
            schedule_compaction(inner);
        }
        if applied {
            persist(inner, true);
            maybe_collapse(inner);
        } else if due {
            persist(inner, false);
        }
    }

    fn push_open(&self, message: Value) {
        let mut state = lock_state(&self.inner);
        state.approx_tokens = state
            .approx_tokens
            .saturating_add(estimate_message_tokens(&message));
        state.open_group.push(message);
        // Content changed: the next `messages()` must rebuild, and any reader
        // gating on `version()` must recompute.
        state.messages_cache = None;
        state.version = state.version.saturating_add(1);
    }
}

impl<F: ClawFs + 'static> Drop for GroupGuard<F> {
    fn drop(&mut self) {
        self.commit();
    }
}

/// Allocate the next monotonic id.
fn next_id(state: &mut MemoryState) -> RecordId {
    let id = state.next_id;
    state.next_id = id.next();
    id
}

/// Serialize `record` to a data line and queue it for the next append.
fn enqueue(
    state: &mut MemoryState,
    record: &LogRecord,
    target: PendingTarget,
    conversation_id: usize,
) {
    match serde_json::to_vec(record) {
        Ok(mut line) => {
            line.push(b'\n');
            state.pending.push(Pending { line, target });
        }
        Err(err) => log::warn!("conversation {conversation_id}: serialize record failed: {err}"),
    }
}

/// How many of the newest groups form the verbatim tail under `keep_recent_tokens`.
///
/// Accumulates tokens newest-first, stopping when the budget is met. Always
/// returns at least 1 (when groups is non-empty) so there is always something
/// verbatim to give the model.
fn compute_verbatim_count(state: &MemoryState, keep_recent_tokens: usize) -> usize {
    if state.groups.is_empty() {
        return 0;
    }
    let mut tokens = 0usize;
    let mut count = 0usize;
    for group in state.groups.iter().rev() {
        let group_tokens: usize = group.msgs.iter().map(estimate_message_tokens).sum();
        tokens = tokens.saturating_add(group_tokens);
        count = count.saturating_add(1);
        if tokens >= keep_recent_tokens {
            break;
        }
    }
    count.max(1)
}

/// Compute one compaction chunk on a pool worker and park the result.
///
/// Strategy: find the next window of aged groups not yet covered by a compact
/// segment (advancing-cursor approach), pack up to `segment_token_budget` tokens
/// from that window, and summarise it. The verbatim tail is always excluded.
fn schedule_compaction<F: ClawFs + 'static>(inner: &Arc<MemoryInner<F>>) {
    if inner.compaction_in_flight.swap(true, Ordering::AcqRel) {
        return;
    }
    let task_inner = Arc::clone(inner);
    inner.deps.pool.submit(Box::new(move || {
        compute_compact_chunk(&task_inner);
        task_inner
            .compaction_in_flight
            .store(false, Ordering::Release);
    }));
}

///
/// Returns without touching state if there is nothing to compact.
fn compute_compact_chunk<F: ClawFs + 'static>(inner: &MemoryInner<F>) {
    let keep_recent_tokens = inner.config.keep_recent_tokens;
    let segment_budget = inner.config.segment_token_budget;

    // Snapshot what to compact (brief lock; no mutation).
    let (chunk, id_start, id_end) = {
        let state = lock_state(inner);

        // Cursor: highest id already covered by a compact segment.
        let cursor: Option<RecordId> = state.compacts.iter().map(|c| c.id_end).max();

        let verbatim_count = compute_verbatim_count(&state, keep_recent_tokens);
        let verbatim_start_idx = state.groups.len().saturating_sub(verbatim_count);
        let aged = state.groups.get(..verbatim_start_idx).unwrap_or(&[]);

        // Candidates: aged groups whose ids come after the cursor.
        let candidates: Vec<&StoredGroup> = aged
            .iter()
            .filter(|g| cursor.map_or(true, |c| g.id > c))
            .collect();

        if candidates.is_empty() {
            return;
        }

        // Fill a chunk up to segment_budget tokens.
        let mut chunk_msgs: Vec<Value> = Vec::new();
        let mut chunk_id_end = candidates[0].id;
        let mut tokens = 0usize;

        for group in &candidates {
            let group_tokens: usize = group.msgs.iter().map(estimate_message_tokens).sum();
            if !chunk_msgs.is_empty() && tokens.saturating_add(group_tokens) > segment_budget {
                break;
            }
            chunk_msgs.extend(group.msgs.iter().cloned());
            chunk_id_end = group.id;
            tokens = tokens.saturating_add(group_tokens);
        }

        let chunk_id_start = candidates[0].id;
        (chunk_msgs, chunk_id_start, chunk_id_end)
    };

    if chunk.is_empty() {
        return;
    }

    let summary = match inner.deps.compactor.compact(&chunk) {
        Ok(summary) => summary,
        Err(err) => {
            log::warn!(
                "conversation {}: compaction skipped: {err}",
                inner.conversation_id
            );
            return;
        }
    };

    lock_state(inner).parked_compact = Some(CompactionResult {
        id_start,
        id_end,
        summary,
    });
}

/// Apply a parked compact result on the foreground.
///
/// Retires covered groups, adds the new compact segment, and enqueues the
/// compact record for the next persist.
///
/// # Coverage continuity invariant
///
/// Each parked chunk starts at `cursor + 1` (the oldest aged group not yet
/// covered — see [`compute_compact_chunk`]), so applying it keeps the compact
/// segments **contiguous and abutting the verbatim groups**: the live set
/// always covers the entire id range `[1, next_id)` with no gaps and no
/// overlaps. Nothing here drops a segment — losing the oldest compact would
/// strand the ids it covered (their groups are already retired), leaving a hole
/// in the index. Unbounded summary growth is a job for re-compacting compacts
/// ("leveling"), never for deleting coverage.
fn apply_parked_compact<F: ClawFs + 'static>(inner: &MemoryInner<F>) -> bool {
    let mut state = lock_state(inner);
    let Some(result) = state.parked_compact.take() else {
        return false;
    };
    let (id_start, id_end) = (result.id_start, result.id_end);

    // Retire covered groups and any pending writes for them.
    state.groups.retain(|g| g.id < id_start || g.id > id_end);
    state.pending.retain(|p| match p.target {
        PendingTarget::Group(id) => id < id_start || id > id_end,
        PendingTarget::Compact(_) => true,
    });

    // Insert the new compact segment in sorted order.
    let insert_pos = state.compacts.partition_point(|c| c.id_start < id_start);
    state.compacts.insert(
        insert_pos,
        StoredCompact {
            id_start,
            id_end,
            messages: result.summary.clone(),
            loc: None,
        },
    );

    let record = LogRecord::Compact {
        id_start,
        id_end,
        summary: result.summary,
    };
    enqueue(
        &mut state,
        &record,
        PendingTarget::Compact(id_start),
        inner.conversation_id,
    );
    state.approx_tokens = estimate_state_tokens(&state);
    state.index_dirty = true;
    // Compaction replaced verbatim groups with a summary: invalidate the snapshot
    // and bump the content version so pull-based readers recompute.
    state.messages_cache = None;
    state.version = state.version.saturating_add(1);

    debug_assert!(
        coverage_is_contiguous(&state),
        "compaction left a hole in id coverage: compacts={:?} groups={:?}",
        state
            .compacts
            .iter()
            .map(|c| (c.id_start, c.id_end))
            .collect::<Vec<_>>(),
        state.groups.iter().map(|g| g.id).collect::<Vec<_>>(),
    );

    true
}

/// Whether the live compact segments and verbatim groups cover a contiguous run
/// of ids with no gaps and no overlaps — the invariant the index must satisfy.
///
/// Used by a `debug_assert!` in [`apply_parked_compact`]; cheap and only walks
/// the (small) live set.
fn coverage_is_contiguous(state: &MemoryState) -> bool {
    // Merge compact ranges and single-group ids into one ascending list of
    // (start, end) spans, then check each span begins exactly where the last ended.
    let mut spans: Vec<(RecordId, RecordId)> = Vec::new();
    spans.extend(state.compacts.iter().map(|c| (c.id_start, c.id_end)));
    spans.extend(state.groups.iter().map(|g| (g.id, g.id)));
    spans.sort_by_key(|(start, _)| *start);

    let mut expected_next: Option<RecordId> = None;
    for (start, end) in spans {
        if let Some(next) = expected_next {
            if start != next {
                return false; // gap or overlap
            }
        }
        expected_next = Some(end.next());
    }
    true
}

/// Flush pending records (one `append`) and, when needed, rewrite the manifest.
fn persist<F: ClawFs + 'static>(inner: &MemoryInner<F>, force_manifest: bool) {
    let mut state = lock_state(inner);
    // Pending data always makes the manifest stale: after the append the
    // data log has records the index doesn't know about. Fold that into
    // want_manifest so the two files stay in sync on every write.
    let has_pending = !state.pending.is_empty();
    let want_manifest = force_manifest || state.index_dirty || has_pending;
    if !has_pending && !want_manifest {
        return;
    }

    if !state.pending.is_empty() {
        let mut data_buf = Vec::new();
        let mut locs = Vec::with_capacity(state.pending.len());
        let mut off = state.data_len.as_offset();
        for pending in &state.pending {
            let len = ByteLen::of(&pending.line);
            data_buf.extend_from_slice(&pending.line);
            locs.push((pending.target, off, len));
            off = off.advance(len);
        }
        if let Err(err) = inner.deps.fs.append(&inner.data_path, &data_buf) {
            log::warn!(
                "conversation {}: data append failed: {err}",
                inner.conversation_id
            );
            return;
        }
        state.data_len = off.as_len();
        for (target, off, len) in locs {
            set_loc(&mut state, target, off, len);
        }
        state.pending.clear();
    }
    state.last_persist = Some(Instant::now());

    if want_manifest {
        if let Some(bytes) = build_manifest_bytes(&state, inner.conversation_id) {
            match inner.deps.fs.write_atomic(&inner.index_path, &bytes) {
                Ok(()) => {
                    state.manifest_covered_len = state.data_len;
                    state.index_dirty = false;
                }
                Err(err) => {
                    log::warn!(
                        "conversation {}: index write failed: {err}",
                        inner.conversation_id
                    );
                    state.index_dirty = true;
                }
            }
        }
    }
}

/// Rewrite both files when dead bytes dominate.
fn maybe_collapse<F: ClawFs + 'static>(inner: &MemoryInner<F>) {
    let collapse = {
        let state = lock_state(inner);
        let live = live_bytes(&state);
        state.data_len > COLLAPSE_FLOOR_BYTES && state.data_len > live.saturating_mul(2)
    };
    if collapse {
        collapse_locked(inner);
    }
}

/// Rewrite `.jsonl` + `.json` from the in-memory live set, updating state locs
/// to the new layout on success. Compact segments are written first (sorted by
/// id_start), then groups in id order.
fn write_live_set_to_files(
    fs: &impl ClawFs,
    data_path: &str,
    index_path: &str,
    state: &mut MemoryState,
    conversation_id: usize,
) {
    let mut data_buf = Vec::new();
    let mut live = Vec::new();
    let mut locs: Vec<(PendingTarget, ByteOffset, ByteLen)> = Vec::new();
    let mut off = ByteOffset::default();

    for compact in &state.compacts {
        let record = LogRecord::Compact {
            id_start: compact.id_start,
            id_end: compact.id_end,
            summary: compact.messages.clone(),
        };
        let Some(len) = append_line(&mut data_buf, &record, conversation_id) else {
            return;
        };
        live.push(IndexEntry::Compact {
            off,
            len,
            id_start: compact.id_start,
            id_end: compact.id_end,
        });
        locs.push((PendingTarget::Compact(compact.id_start), off, len));
        off = off.advance(len);
    }
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
        locs.push((PendingTarget::Group(group.id), off, len));
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
    state.index_dirty = false;
    for compact in &mut state.compacts {
        compact.loc = None;
    }
    for group in &mut state.groups {
        group.loc = None;
    }
    for (target, off, len) in locs {
        set_loc(state, target, off, len);
    }
}

fn collapse_locked<F: ClawFs + 'static>(inner: &MemoryInner<F>) {
    let mut state = lock_state(inner);
    write_live_set_to_files(
        &inner.deps.fs,
        &inner.data_path,
        &inner.index_path,
        &mut state,
        inner.conversation_id,
    );
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

/// Record a flushed record's byte location back onto its in-memory entry.
fn set_loc(state: &mut MemoryState, target: PendingTarget, off: ByteOffset, len: ByteLen) {
    match target {
        PendingTarget::Group(id) => {
            if let Some(group) = state.groups.iter_mut().find(|g| g.id == id) {
                group.loc = Some((off, len));
            }
        }
        PendingTarget::Compact(id_start) => {
            if let Some(compact) = state.compacts.iter_mut().find(|c| c.id_start == id_start) {
                compact.loc = Some((off, len));
            }
        }
    }
}

/// Build the manifest of the current live records (those already on disk).
fn build_manifest_bytes(state: &MemoryState, conversation_id: usize) -> Option<Vec<u8>> {
    let mut live = Vec::new();
    for compact in &state.compacts {
        if let Some((off, len)) = compact.loc {
            live.push(IndexEntry::Compact {
                off,
                len,
                id_start: compact.id_start,
                id_end: compact.id_end,
            });
        }
    }
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

/// Total bytes of records currently considered live and present on disk.
fn live_bytes(state: &MemoryState) -> ByteLen {
    let mut total = ByteLen::default();
    for compact in &state.compacts {
        if let Some((_, len)) = compact.loc {
            total = total.saturating_add(len);
        }
    }
    for group in &state.groups {
        if let Some((_, len)) = group.loc {
            total = total.saturating_add(len);
        }
    }
    total
}

fn persist_due<F: ClawFs + 'static>(inner: &MemoryInner<F>, state: &MemoryState) -> bool {
    if state.pending.is_empty() && !state.index_dirty {
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
        (
            IndexEntry::Compact {
                id_start: es,
                id_end: ee,
                ..
            },
            LogRecord::Compact {
                id_start: rs,
                id_end: re,
                ..
            },
        ) => es == rs && ee == re,
        _ => false,
    }
}

/// Load and rehydrate persisted state. Returns `(state, needs_rebuild)`.
/// `needs_rebuild` is true when a manifest existed but its entries did not match
/// the data log — the caller should rewrite both files from the recovered state.
fn load_state(fs: &impl ClawFs, data_path: &str, index_path: &str) -> (MemoryState, bool) {
    let mut state = MemoryState::default();
    let mut covered_len = ByteLen::default();
    let mut manifest_next_id = RecordId::default();
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
        state = MemoryState::default();
        covered_len = ByteLen::default();
        manifest_next_id = RecordId::default();
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
        state.index_dirty = true;
    }

    state.data_len = data_len;
    state.manifest_covered_len = covered_len;
    state.groups.sort_by_key(|g| g.id);
    state.compacts.sort_by_key(|c| c.id_start);
    state.next_id = manifest_next_id.max(max_seen_id(&state).next());
    state.approx_tokens = estimate_state_tokens(&state);
    (state, mismatch)
}

/// Parse a newline-delimited tail buffer, applying each complete record.
fn scan_tail(state: &mut MemoryState, tail: &[u8], base_off: ByteOffset) {
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

/// Fold one decoded record into the live state, applying supersession.
///
/// Supersession is keyed on [`RecordId`] only; `loc` is the byte address of the
/// record and is never used to decide order.
fn apply_record(state: &mut MemoryState, record: LogRecord, loc: Option<(ByteOffset, ByteLen)>) {
    match record {
        LogRecord::Group { id, msgs } => {
            // A group already covered by a compact segment is a dead record
            // (superseded). Skip it rather than adding and immediately removing it.
            let covered = state
                .compacts
                .iter()
                .any(|c| id >= c.id_start && id <= c.id_end);
            if !covered {
                state.groups.push(StoredGroup { id, msgs, loc });
            }
        }
        LogRecord::Compact {
            id_start,
            id_end,
            summary,
        } => {
            // Retire any groups now covered by this compact segment.
            state.groups.retain(|g| g.id < id_start || g.id > id_end);
            state.compacts.push(StoredCompact {
                id_start,
                id_end,
                messages: summary,
                loc,
            });
        }
    }
}

/// Highest id currently represented (live group or compact range end).
fn max_seen_id(state: &MemoryState) -> RecordId {
    let group_max = state.groups.iter().map(|g| g.id).max().unwrap_or_default();
    let compact_max = state
        .compacts
        .iter()
        .map(|c| c.id_end)
        .max()
        .unwrap_or_default();
    group_max.max(compact_max)
}

fn entry_loc(entry: &IndexEntry) -> (ByteOffset, ByteLen) {
    match *entry {
        IndexEntry::Group { off, len, .. } | IndexEntry::Compact { off, len, .. } => (off, len),
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

fn lock_state<F: ClawFs + 'static>(inner: &MemoryInner<F>) -> MutexGuard<'_, MemoryState> {
    inner
        .state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

// todo: replace this byte-length heuristic with a real tokenizer estimate that
// matches the active backend's accounting. It only needs to be monotonic and
// roughly proportional for the compaction trigger to behave.
fn estimate_message_tokens(message: &Value) -> usize {
    message.to_string().len() / CHARS_PER_TOKEN + 1
}

fn estimate_state_tokens(state: &MemoryState) -> usize {
    let compacts: usize = state
        .compacts
        .iter()
        .flat_map(|c| c.messages.iter())
        .map(estimate_message_tokens)
        .sum();
    let groups: usize = state
        .groups
        .iter()
        .flat_map(|g| g.msgs.iter())
        .map(estimate_message_tokens)
        .sum();
    let open: usize = state.open_group.iter().map(estimate_message_tokens).sum();
    compacts.saturating_add(groups).saturating_add(open)
}
