//! The three traits the agent's context plumbing rests on: the [`History`] read
//! view over the conversation transcript, the [`Transcript`] write face
//! [`TranscriptStore`](claw_memory::TranscriptStore) exposes to the agent, and the
//! pull-based [`ContextAdapter`] every pluggable context source implements.
//!
//! # One transcript, owned by `TranscriptStore`; adapters only read it
//!
//! The conversation transcript has a single owner — the
//! [`TranscriptStore`](claw_memory::TranscriptStore) — which exposes:
//! - [`History`] (read): the message snapshot + a change [`version`](History::version),
//! - [`Transcript`] (write): the boundary writes the agent drives directly.
//!
//! Every [`ContextAdapter`] contributes first-class context items into a
//! `claw-context` sink and may
//! schedule its own background work into its own store. There is no event bus and
//! nothing is pushed at an adapter — an adapter pulls the source views it needs
//! during [`contribute`](ContextAdapter::contribute) and self-detects change by
//! comparing source versions to cursors it keeps.

use core::future::Future;
use core::pin::Pin;
use std::sync::Arc;

use claw_context::ContextSink;
use claw_interface::ClawFs;
use claw_memory::TranscriptStore;
use claw_tool::ToolGroup;
use serde_json::{json, Value};

/// The read view of the conversation transcript: the one interface request
/// assembly — and every pluggable context adapter — needs from the transcript owner.
///
/// Handed around as `&dyn History` so a reader depends on this narrow view,
/// never on the concrete conversation-memory type (which would drag its
/// storage/compaction/persistence — and a filesystem type parameter — along).
///
/// Both request assembly and every pluggable [`ContextAdapter`] read through it.
pub(crate) trait History {
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

/// Assistant message shape to commit into the transcript.
pub(crate) enum AssistantCommit<'a> {
    /// Backend-shaped assistant message JSON returned by the LLM.
    RawJson(&'a str),
    /// Plain assistant text; the transcript layer wraps it as an assistant
    /// message object.
    PlainText(&'a str),
}

/// The write face of the transcript owner, driven by the agent at each turn
/// boundary. Extends [`History`]: one object reads and writes the single
/// transcript, so a write is always visible to the next read.
///
/// The agent holds this as `Arc<dyn Transcript>` and writes through it directly
/// (no event indirection); it lends the read view to adapters via
/// [`as_history`](Self::as_history).
pub(crate) trait Transcript: History {
    /// Append a user message, reusing the open turn or opening a new one. When
    /// `starts_task`, first close out any turn a prior task left open so the new
    /// task starts a fresh group.
    fn append_user(&self, text: &str, starts_task: bool);

    /// Commit the model's answer, closing the open turn.
    fn commit_assistant(&self, commit: AssistantCommit<'_>);

    /// Append a materialized assistant+tool patch (a JSON array) into the open
    /// turn **without** closing it.
    ///
    /// A single user request can span several iterations (each tool round is its
    /// own iteration). Those intermediate rounds append here so the whole request
    /// stays one user-started turn; only the terminal iteration
    /// ([`commit_assistant`](Self::commit_assistant) /
    /// [`commit_ended`](Self::commit_ended)) — or the next task's
    /// [`append_user`](Self::append_user) — closes the turn.
    fn append_patch(&self, patch: &Value);

    /// Commit the agent's closing message (from `conversation_end`).
    fn commit_ended(&self, final_message: &str);

    /// Discard the still-open turn without committing any of its messages.
    ///
    /// Used by hard cancellation: cancelled work should not leak partial user,
    /// assistant, or tool messages into later model context.
    fn discard_open_turn(&self);

    /// Borrow the read-only [`History`] view to hand to adapters.
    ///
    /// A plain unsizing coercion (`self`), provided as a method so the agent need
    /// not rely on trait upcasting from `&dyn Transcript` to `&dyn History`.
    fn as_history(&self) -> &dyn History;
}

impl<F: ClawFs + 'static> History for TranscriptStore<F> {
    fn messages(&self) -> Arc<Value> {
        TranscriptStore::messages(self)
    }

    fn version(&self) -> u64 {
        TranscriptStore::version(self)
    }
}

impl<F: ClawFs + 'static> Transcript for TranscriptStore<F> {
    fn append_user(&self, text: &str, starts_task: bool) {
        if starts_task {
            self.commit_open_turn();
        }
        self.push_user_message(text);
    }

    fn commit_assistant(&self, commit: AssistantCommit<'_>) {
        match commit {
            AssistantCommit::RawJson(raw) => self.push_assistant_message(raw),
            AssistantCommit::PlainText(text) => {
                self.push_patch(&json!([{ "role": "assistant", "content": text }]));
            }
        }
        self.commit_open_turn();
    }

    fn append_patch(&self, patch: &Value) {
        self.push_patch(patch);
    }

    fn commit_ended(&self, final_message: &str) {
        self.push_patch(&json!([{ "role": "assistant", "content": final_message }]));
        self.commit_open_turn();
    }

    fn discard_open_turn(&self) {
        TranscriptStore::discard_open_turn(self);
    }

    fn as_history(&self) -> &dyn History {
        self
    }
}

/// Future returned by [`ContextAdapter::prepare`].
pub(super) type ContextAdapterFuture<'a> = Pin<Box<dyn Future<Output = ()> + 'a>>;

/// A pluggable source that contributes to the next LLM request and may provide
/// model-callable tools.
///
/// Owned by the agent (one `Box<dyn ContextAdapter>` per registration) and driven
/// from its single tick thread.
///
/// The agent does not decide whether a source is a system block, history message,
/// or ephemeral reminder; each adapter emits the correct item into the sink and
/// `claw-context` owns placement, ordering, and render caches.
pub(crate) trait ContextAdapter {
    /// Refresh any async state needed for the next contribution.
    ///
    /// Called from the agent's local async tick before [`contribute`](Self::contribute).
    /// The default is a no-op for purely synchronous projectors.
    fn prepare<'a>(&'a mut self, _history: &'a dyn History) -> ContextAdapterFuture<'a> {
        Box::pin(async {})
    }

    /// Project this source into the request context for the current iteration.
    fn contribute(&mut self, output: &mut ContextSink<'_>);

    /// The model-callable tools this adapter provides.
    ///
    /// Added into the agent's tool set when the adapter is registered. Tool names
    /// must be globally unique across the agent's tools (a clash is rejected at
    /// registration). The default provides no tools.
    fn tools(&self) -> Option<ToolGroup> {
        None
    }
}
