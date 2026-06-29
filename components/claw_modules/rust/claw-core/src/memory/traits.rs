//! The three memory traits: the [`History`] read view over the conversation
//! transcript, the [`Transcript`] write face its owner exposes to the agent, and
//! the pull-based [`Memory`] every pluggable memory implements.
//!
//! # One transcript, owned by `History`; memories only read it
//!
//! The conversation transcript has a single owner — the
//! [`ConversationHistory`](super::ConversationHistory) — which exposes:
//! - [`History`] (read): the message snapshot + a change [`version`](History::version),
//! - [`Transcript`] (write): the boundary writes the agent drives directly.
//!
//! Every other memory is a **pure reader**: given a `&dyn History`, it derives
//! context blocks (and may schedule its own background work into its own store).
//! There is no event bus and nothing is pushed at a memory — a memory *pulls* the
//! transcript when the agent asks it to [`render_context`](Memory::render_context),
//! and self-detects change by comparing [`History::version`] to a cursor it keeps.
//! This keeps the agent ignorant of every concrete memory type and makes "the
//! transcript lives in exactly one place, everyone else borrows it" literally true.

use std::sync::Arc;

use claw_context::Block;
use claw_tool::ToolGroup;
use serde_json::Value;

/// The read view of the conversation transcript: the one capability request
/// assembly — and every pluggable [`Memory`] — needs from the transcript owner.
///
/// Handed around as `&dyn History` so a reader depends on this narrow capability,
/// never on the concrete conversation-memory type (which would drag its
/// storage/compaction/persistence — and a filesystem type parameter — along).
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
/// (no event indirection); it lends the read view to memories via
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

    /// Borrow the read-only [`History`] view to hand to memories.
    ///
    /// A plain unsizing coercion (`self`), provided as a method so the agent need
    /// not rely on trait upcasting from `&dyn Transcript` to `&dyn History`.
    fn as_history(&self) -> &dyn History;
}

/// A pluggable memory: a pure reader over [`History`] that contributes context
/// blocks and may provide model-callable tools.
///
/// Implementations are shared as `Arc<dyn Memory>` and driven from the agent's
/// single tick thread, but must be `Send + Sync` because a memory typically owns
/// state also written by background workers (e.g. an extraction pool job).
///
/// # Two facets, both optional
///
/// - **contribute context** — [`render_context`](Self::render_context) reads the
///   lent transcript, optionally schedules its own background work, and emits
///   [`Block`]s into the agent's [`Context`](claw_context::Context);
/// - **provide tools** — [`tools`](Self::tools) hands the agent model-callable
///   tools (e.g. `memory_recall`) merged in at registration.
pub trait Memory: Send + Sync {
    /// A stable identifier for this memory, used in logs.
    fn id(&self) -> &str;

    /// Contribute context blocks for the current iteration by calling `emit` once
    /// per block.
    ///
    /// Called by the agent's tick thread just before it assembles the request, so
    /// a memory shared across agents (and written by background workers) is read
    /// into *this* agent's [`Context`](claw_context::Context) only here, on the
    /// thread that owns it. `history` is the lent transcript read view: a memory
    /// derives its blocks from the conversation here, and gates any background
    /// work on [`History::version`]. Emitting an empty block removes that block
    /// (the context drops blank content), so a memory can clear a section by
    /// emitting it empty. The default emits nothing.
    fn render_context(&self, history: &dyn History, emit: &mut dyn FnMut(Block<'_>)) {
        let _ = (history, emit);
    }

    /// The model-callable tools this memory provides, if any.
    ///
    /// Merged into the agent's tool set when the memory is registered. Tool names
    /// must be globally unique across the agent's tools (a clash is rejected at
    /// registration). The default provides none.
    fn tools(&self) -> Option<ToolGroup> {
        None
    }
}
