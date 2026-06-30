//! The three traits the agent's context plumbing rests on: the [`History`] read
//! view over the conversation transcript, the [`Transcript`] write face its owner
//! exposes to the agent, and the pull-based [`ContextAdapter`] every pluggable
//! context source implements.
//!
//! # One transcript, owned by `History`; adapters only read it
//!
//! The conversation transcript has a single owner — the
//! [`ConversationHistory`](super::ConversationHistory) — which exposes:
//! - [`History`] (read): the message snapshot + a change [`version`](History::version),
//! - [`Transcript`] (write): the boundary writes the agent drives directly.
//!
//! Every [`ContextAdapter`] is a **pure reader**: given a `&dyn History`, it
//! derives context (text blocks and/or structured messages) and may schedule its
//! own background work into its own store. There is no event bus and nothing is
//! pushed at an adapter — an adapter *pulls* the transcript when the agent
//! [`refresh`](ContextAdapter::refresh)es it, and self-detects change by comparing
//! [`History::version`] to a cursor it keeps. This keeps the agent ignorant of
//! every concrete adapter type and makes "the transcript lives in exactly one
//! place, everyone else borrows it" literally true.

use std::sync::Arc;

use claw_context::{Block, BlockKind};
use claw_tool::ToolGroup;
use serde_json::Value;

/// The read view of the conversation transcript: the one capability request
/// assembly — and every pluggable [`Memory`] — needs from the transcript owner.
///
/// Handed around as `&dyn History` so a reader depends on this narrow capability,
/// never on the concrete conversation-memory type (which would drag its
/// storage/compaction/persistence — and a filesystem type parameter — along).
///
/// Both request assembly and every pluggable [`ContextAdapter`] read through it.
pub trait History: Send + Sync {
    /// The current transcript as a JSON array of chat messages.
    ///
    /// Returns an [`Arc`] so the snapshot is shared, not deep-copied, on every
    /// read (the transcript can be large and is read once per request). The owner
    /// caches it and rebuilds only when the content changes, so an unchanged
    /// transcript hands back a refcount bump.
    fn messages(&self) -> Arc<Value>;

    /// A monotonic counter bumped whenever [`messages`](Self::messages) output
    /// changes.
    ///
    /// A pull-based memory caches its derived output (or gates background work)
    /// on this and recomputes only when it advances, instead of diffing the
    /// transcript.
    fn version(&self) -> u64;
}

/// The write face of the transcript owner, driven by the agent at each turn
/// boundary. Extends [`History`]: one object reads and writes the single
/// transcript, so a write is always visible to the next read.
///
/// The agent holds this as `Arc<dyn Transcript>` and writes through it directly
/// (no event indirection); it lends the read view to adapters via
/// [`as_history`](Self::as_history).
pub trait Transcript: History {
    /// Append a user message, reusing the open turn or opening a new one. When
    /// `starts_task`, first close out any turn a prior task left open so the new
    /// task starts a fresh group.
    fn append_user(&self, text: &str, starts_task: bool);

    /// Commit the model's plain-text answer, closing the open turn. Uses the raw
    /// assistant message JSON when present, else builds one from `text`.
    fn commit_assistant(&self, text: &str, raw_json: Option<&str>);

    /// Commit a materialized assistant+tool patch (a JSON array), closing the
    /// open turn.
    fn commit_patch(&self, patch: &Value);

    /// Commit the agent's closing message (from `end_conversation`).
    fn commit_ended(&self, final_message: &str);

    /// Record a cancellation `marker` as a user message and commit it together
    /// with any abandoned (still-open) turn.
    fn commit_cancellation(&self, marker: &str);

    /// Borrow the read-only [`History`] view to hand to adapters.
    ///
    /// A plain unsizing coercion (`self`), provided as a method so the agent need
    /// not rely on trait upcasting from `&dyn Transcript` to `&dyn History`.
    fn as_history(&self) -> &dyn History;
}

/// A pluggable context source: a pure reader over [`History`] that contributes
/// to the next LLM request — text [`Block`]s into the system prefix and/or
/// structured messages into the history channel — and may provide model-callable
/// tools.
///
/// Owned by the agent (one `Box<dyn ContextAdapter>` per registration) and driven
/// from its single tick thread, but `Send + Sync` because an adapter typically
/// owns state also written by background workers (e.g. an extraction pool job),
/// and the agent itself moves across worker tasks.
///
/// # Two-phase per iteration: refresh, then lend
///
/// The agent drives every adapter once per iteration in two passes, so the read
/// borrows of *all* adapters can coexist while the agent collects from them:
///
/// 1. [`refresh`](Self::refresh) (`&mut self`): read the lent transcript,
///    recompute the adapter's internal cache, and schedule any background work.
///    Exclusive access means the cache is plain fields — no interior locking.
/// 2. [`blocks`](Self::blocks) / [`messages`](Self::messages) (`&self`): lend the
///    just-refreshed contributions, borrowing the adapter's cache (zero-copy).
///
/// Both contribution methods default to empty, so an adapter implements only the
/// channel(s) it feeds. The agent orders contributions across adapters by
/// [`BlockKind`] (the block taxonomy is the single wire-order authority).
pub trait ContextAdapter: Send + Sync {
    /// A stable identifier for this adapter, used in logs.
    fn id(&self) -> &str;

    /// Read the lent transcript and refresh internal state for this iteration.
    ///
    /// Called first, on the agent's tick thread, before either contribution
    /// method. `transcript` is the lent read view: the adapter derives its cached
    /// output here and gates any background work on [`History::version`] (an
    /// unchanged transcript should cost nothing). `&mut self` so the cache is
    /// plain fields, never a lock.
    fn refresh(&mut self, transcript: &dyn History);

    /// Lend the text blocks this adapter contributes to the system prefix, each
    /// tagged with its [`BlockKind`]. Borrows the cache refreshed by
    /// [`refresh`](Self::refresh); an empty block clears that section (the context
    /// drops blank content). The default contributes none.
    fn blocks(&self) -> Vec<Block<'_>> {
        Vec::new()
    }

    /// Lend the structured messages this adapter contributes to the history
    /// channel, each tagged with the [`BlockKind`] that fixes its wire position
    /// (e.g. `ConversationSummary` before `RecentContext`). The agent merges these
    /// across adapters and orders them by `kind.sort_key()`. The default
    /// contributes none.
    fn messages(&self) -> Vec<(BlockKind, &Value)> {
        Vec::new()
    }

    /// The model-callable tools this adapter provides, if any.
    ///
    /// Merged into the agent's tool set when the adapter is registered. Tool names
    /// must be globally unique across the agent's tools (a clash is rejected at
    /// registration). The default provides none.
    fn tools(&self) -> Option<ToolGroup> {
        None
    }
}
