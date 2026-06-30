//! [`ConversationHistory`] — the sole owner of the conversation transcript.
//!
//! The transcript is the agent's authoritative history. This type owns it and
//! exposes the two faces the agent needs, over one underlying [`TranscriptStore`]:
//!
//! - **read** — [`History`]: the message snapshot + a change version, lent to the
//!   agent for request assembly and to every [`ContextAdapter`](crate::memory::ContextAdapter)
//!   for pulling.
//! - **write** — [`Transcript`]: the boundary writes the agent drives directly
//!   (a user message, a committed answer or tool patch, an end/cancel marker).
//!
//! It is deliberately **not** a context adapter: the transcript is the thing
//! adapters read *from*, not one of the readers. The agent holds it as
//! `Arc<dyn Transcript>` and never sees the concrete store — which is the only
//! place the filesystem type parameter `F` is erased.
//!
//! # Turn grouping
//!
//! A [`GroupGuard`] batches a user turn and the assistant reply that answers it
//! into one turn group. This type owns the open guard:
//! [`append_user`](Transcript::append_user) opens or reuses it, and an
//! assistant / tool / end / cancel commit takes (closes) it. `starts_task`
//! flushes any guard left open by a previous task (e.g. one that failed mid-turn)
//! before opening the new turn.

use std::sync::{Arc, Mutex};

use claw_interface::ClawFs;
use claw_memory::{GroupGuard, TranscriptStore};
use serde_json::{json, Value};

use crate::memory::traits::{History, Transcript};

/// The owner of the conversation transcript: a [`History`] + [`Transcript`] over
/// one [`TranscriptStore`]. See the module docs.
///
/// Holds the agent's transcript store (the same `Arc`-backed store a caller may
/// keep a read clone of) plus the open turn-group guard. Driven only from the
/// agent's tick thread; the guard sits behind a `Mutex` solely to satisfy the
/// `Send + Sync` bound the `Arc<dyn Transcript>` trait object requires.
pub struct ConversationHistory<F: ClawFs + 'static> {
    store: TranscriptStore<F>,
    /// The open turn group, from a user message until the reply that closes it.
    open_turn: Mutex<Option<GroupGuard<F>>>,
}

impl<F: ClawFs + 'static> ConversationHistory<F> {
    /// Wrap the agent's transcript store as the transcript owner.
    pub fn new(store: TranscriptStore<F>) -> Self {
        Self {
            store,
            open_turn: Mutex::new(None),
        }
    }

    fn lock_turn(&self) -> std::sync::MutexGuard<'_, Option<GroupGuard<F>>> {
        self.open_turn
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Take the open turn (closing it on commit) or open a fresh group.
    fn take_or_open(&self) -> GroupGuard<F> {
        self.lock_turn()
            .take()
            .unwrap_or_else(|| self.store.group())
    }
}

impl<F: ClawFs + 'static> History for ConversationHistory<F> {
    fn messages(&self) -> Arc<Value> {
        self.store.messages()
    }

    fn version(&self) -> u64 {
        self.store.version()
    }
}

impl<F: ClawFs + 'static> Transcript for ConversationHistory<F> {
    fn append_user(&self, text: &str, starts_task: bool) {
        let mut open = self.lock_turn();
        if starts_task {
            *open = None;
        }
        match open.as_ref() {
            Some(turn) => turn.append_user(text),
            None => {
                let turn = self.store.group();
                turn.append_user(text);
                *open = Some(turn);
            }
        }
    }

    fn commit_assistant(&self, text: &str, raw_json: Option<&str>) {
        let turn = self.take_or_open();
        match raw_json {
            Some(raw) => turn.append_assistant(raw),
            None => turn.append_patch(&json!([{ "role": "assistant", "content": text }])),
        }
    }

    fn commit_patch(&self, patch: &Value) {
        self.take_or_open().append_patch(patch);
    }

    fn commit_ended(&self, final_message: &str) {
        self.take_or_open()
            .append_patch(&json!([{ "role": "assistant", "content": final_message }]));
    }

    fn commit_cancellation(&self, marker: &str) {
        let turn = self.take_or_open();
        turn.append_user(marker);
        // `turn` drops here, committing the abandoned turn plus the marker.
    }

    fn as_history(&self) -> &dyn History {
        self
    }
}
