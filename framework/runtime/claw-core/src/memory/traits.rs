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
//! Every [`ContextAdapter`] is a **pure projector**: given a narrow input view, it
//! contributes first-class context items into a `claw-context` sink and may
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
use claw_tool::{ToolGroup, ToolSet};
use serde_json::{json, Value};

/// The read view of the conversation transcript: the one capability request
/// assembly — and every pluggable context adapter — needs from the transcript owner.
///
/// Handed around as `&dyn History` so a reader depends on this narrow capability,
/// never on the concrete conversation-memory type (which would drag its
/// storage/compaction/persistence — and a filesystem type parameter — along).
///
/// Both request assembly and every pluggable [`ContextAdapter`] read through it.
pub trait History {
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
pub enum AssistantCommit<'a> {
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
pub trait Transcript: History {
    /// Append a user message, reusing the open turn or opening a new one. When
    /// `starts_task`, first close out any turn a prior task left open so the new
    /// task starts a fresh group.
    fn append_user(&self, text: &str, starts_task: bool);

    /// Commit the model's answer, closing the open turn.
    fn commit_assistant(&self, commit: AssistantCommit<'_>);

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

    fn commit_patch(&self, patch: &Value) {
        self.push_patch(patch);
        self.commit_open_turn();
    }

    fn commit_ended(&self, final_message: &str) {
        self.push_patch(&json!([{ "role": "assistant", "content": final_message }]));
        self.commit_open_turn();
    }

    fn commit_cancellation(&self, marker: &str) {
        self.push_user_message(marker);
        self.commit_open_turn();
    }

    fn as_history(&self) -> &dyn History {
        self
    }
}

/// The read-only runtime sources an adapter may project into context.
#[derive(Clone, Copy)]
pub struct ContextAdapterInput<'a> {
    /// The conversation transcript read view.
    pub history: &'a dyn History,
    /// The current tool set. It may be empty when this agent has no tools.
    pub tools: &'a ToolSet,
}

/// Future returned by [`ContextAdapter::prepare`].
pub type ContextAdapterFuture<'a> = Pin<Box<dyn Future<Output = ()> + 'a>>;

/// A pluggable context source: a pure projector over [`ContextAdapterInput`] that
/// contributes to the next LLM request through a `claw-context` [`ContextSink`],
/// and may provide model-callable tools.
///
/// Owned by the agent (one `Box<dyn ContextAdapter>` per registration) and driven
/// from its single tick thread.
///
/// The agent does not decide whether a source is a system block, history message,
/// or ephemeral reminder; each adapter emits the correct item into the sink and
/// `claw-context` owns placement, ordering, and render caches.
pub trait ContextAdapter {
    /// A stable identifier for this adapter, used in logs.
    fn id(&self) -> &str;

    /// Refresh any async state needed for the next contribution.
    ///
    /// Called from the agent's local async tick before [`contribute`](Self::contribute).
    /// The default is a no-op for purely synchronous projectors.
    fn prepare<'a>(&'a mut self, _input: ContextAdapterInput<'a>) -> ContextAdapterFuture<'a> {
        Box::pin(async {})
    }

    /// Project this source into the request context for the current iteration.
    fn contribute(&mut self, input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>);

    /// The model-callable tool groups this adapter provides.
    ///
    /// Merged into the agent's tool set when the adapter is registered. Tool names
    /// must be globally unique across the agent's tools (a clash is rejected at
    /// registration). The default provides no groups.
    fn tools(&self) -> Vec<ToolGroup> {
        Vec::new()
    }
}
