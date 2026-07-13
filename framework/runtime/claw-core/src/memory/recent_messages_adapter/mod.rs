//! [`RecentMessagesContextAdapter`] — the verbatim recent-history half of the
//! conversation, contributed to the model's structured `messages` channel.
//!
//! It is the [`ContextAdapter`] that turns the conversation transcript's
//! *verbatim tail* into history messages tagged [`BlockKind::RecentContext`]. The
//! tail is the committed turns *after* the [`SummaryCursor`] (the ones the rolling
//! summary has not folded into a summary yet), plus the in-progress open turn.
//! The older, summarized prefix is the sibling
//! [`RollingSummaryContextAdapter`](super::RollingSummaryContextAdapter)'s job;
//! the agent merges the two channels and orders them by
//! [`BlockKind::sort_key`](claw_context::BlockKind::sort_key) so summaries always
//! precede the recent tail.
//!
//! # Why the cursor
//!
//! Without it this adapter would render *every* verbatim turn, duplicating the
//! turns the summary already covers. The shared [`SummaryCursor`] is the boundary:
//! the summary adapter advances it as it summarizes, and this adapter renders only
//! what lies beyond it. Reads are gated on both the transcript
//! [`version`](claw_memory::TranscriptStore::version) and the cursor value, so an
//! unchanged boundary re-lends the cached snapshot with no work.

use std::sync::Arc;

use claw_context::{BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{TranscriptStore, TurnId};
use serde_json::Value;

use crate::memory::summary_cursor::SummaryCursor;
use crate::memory::traits::{ContextAdapter, ContextAdapterInput};

/// Contributes the conversation's verbatim recent tail to the history channel.
/// See the module docs.
pub(crate) struct RecentMessagesContextAdapter<F: ClawFs + 'static> {
    /// A clone of the agent's transcript store (shares the same `Arc`-backed
    /// state the transcript writes to), read for its committed turns + open turn.
    store: TranscriptStore<F>,
    /// The boundary: render committed turns whose id is past this. Advanced by the
    /// sibling rolling-summary adapter; read-only here.
    cursor: SummaryCursor,
    /// Cached verbatim-tail snapshot, rebuilt during
    /// [`contribute`](ContextAdapter::contribute) only when the transcript version
    /// or the cursor advances, then emitted into the context sink.
    cached: Arc<Value>,
    /// The store [`version`](TranscriptStore::version) `cached` reflects.
    cached_version: u64,
    /// The [`SummaryCursor`] value `cached` reflects.
    cached_cursor: Option<TurnId>,
    /// `false` until the first refresh fills `cached` (version 0 is a real, empty
    /// state, so a flag distinguishes "never refreshed").
    primed: bool,
}

impl<F: ClawFs + 'static> RecentMessagesContextAdapter<F> {
    /// Build the adapter over a clone of the agent's transcript `store` and the
    /// shared `cursor` the rolling-summary adapter advances.
    pub(crate) fn new(store: TranscriptStore<F>, cursor: SummaryCursor) -> Self {
        Self {
            store,
            cursor,
            cached: Arc::new(Value::Array(Vec::new())),
            cached_version: 0,
            cached_cursor: None,
            primed: false,
        }
    }

    fn refresh_tail(&mut self) {
        // The lent transcript and `self.store` share the same `Arc`-backed state,
        // so gate on the store's version (the same source the snapshot reads from)
        // together with the cursor the summary adapter advances.
        let version = self.store.version();
        let cursor = self.cursor.covered_through();
        if self.primed && version == self.cached_version && cursor == self.cached_cursor {
            return;
        }
        let turns = self.store.turns_snapshot();
        let mut out = Vec::new();
        for turn in turns
            .iter()
            .filter(|turn| cursor.map_or(true, |covered| turn.id > covered))
        {
            out.extend(turn.messages.iter().cloned());
        }
        // The open turn is always the newest content and is never summarized, so
        // it is always part of the verbatim tail.
        out.extend(self.store.open_turn_messages());
        self.cached = Arc::new(Value::Array(out));
        self.cached_version = version;
        self.cached_cursor = cursor;
        self.primed = true;
    }
}

impl<F: ClawFs + 'static> ContextAdapter for RecentMessagesContextAdapter<F> {
    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        self.refresh_tail();
        if let Some(items) = self.cached.as_array() {
            for message in items {
                output.message(BlockKind::RecentContext, message);
            }
        }
    }
}
