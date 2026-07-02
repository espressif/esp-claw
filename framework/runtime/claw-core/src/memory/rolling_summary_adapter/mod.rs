//! [`RollingSummaryContextAdapter`] — the summarized older-history half of the
//! conversation, and the owner of conversation compaction.
//!
//! Compaction is "fold the aged conversation prefix into a summary so the request
//! fits the model's context window". That is a property of the **LLM request**,
//! not of the stored transcript — so it lives here, in the agent layer, entirely.
//! The [`TranscriptStore`] keeps the full verbatim record and is never mutated by
//! this adapter; this adapter only *reads* it and keeps its own summary.
//!
//! Two jobs, both pull-based off the agent's tick:
//!
//! - **Compact (write its own state):** on each
//!   [`contribute`](ContextAdapter::contribute) it looks at the committed turns
//!   past the shared [`SummaryCursor`] and, once their token estimate crosses the
//!   trigger budget, summarizes the oldest aged turns (keeping a verbatim tail)
//!   by running the injected [`Compactor`] on the agent's local runtime, which
//!   advances the cursor immediately — so the sibling recent-tail adapter stops
//!   rendering the now-summarized turns. No gap, no overlap.
//! - **Render (read):** it surfaces its accumulated summary segments as history
//!   messages tagged [`BlockKind::ConversationSummary`]. The recent verbatim tail
//!   is the sibling
//!   [`RecentMessagesContextAdapter`](super::RecentMessagesContextAdapter)'s job;
//!   the agent merges the two channels and orders them by
//!   [`BlockKind::sort_key`](claw_context::BlockKind::sort_key) so the summary
//!   prefix renders ahead of the recent tail.
//!
//! # Re-derivable, not authoritative
//!
//! The summary is *derived* from the transcript (the source of truth), so it is a
//! cache: it currently lives only in memory and is rebuilt by re-summarizing on a
//! fresh process. Persisting it as a re-derivable cache (to avoid re-summarizing
//! on boot) is a future optimization — losing it can never lose conversation
//! data, since the verbatim turns it covered are still in the store.

use std::sync::Arc;

use claw_context::{BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{Compactor, TranscriptStore, Turn, TurnId};
use serde_json::Value;

use crate::memory::summary_cursor::SummaryCursor;
use crate::memory::traits::{ContextAdapter, ContextAdapterFuture, ContextAdapterInput};

/// Stable id for this adapter, used in logs.
const ADAPTER_ID: &str = "rolling_summary";

/// Rough bytes-per-token divisor for the size estimate. See
/// [`estimate_message_tokens`].
const CHARS_PER_TOKEN: usize = 4;

/// The conversation-compaction policy knobs the adapter applies.
///
/// Built from the agent's memory tuning at construction. The transcript store
/// does not read these — only this adapter does.
#[derive(Clone, Copy, Debug)]
pub struct CompactionPolicy {
    /// Start compacting once the verbatim history past the cursor exceeds this.
    pub trigger_tokens: usize,
    /// Token budget for the verbatim tail kept out of every summary (the newest
    /// turns the model always sees word-for-word).
    pub keep_recent_tokens: usize,
    /// Max tokens summarized per compaction pass (one summary segment).
    pub segment_token_budget: usize,
}

impl CompactionPolicy {
    /// Build a policy from its three token budgets.
    pub fn new(
        trigger_tokens: usize,
        keep_recent_tokens: usize,
        segment_token_budget: usize,
    ) -> Self {
        Self {
            trigger_tokens,
            keep_recent_tokens,
            segment_token_budget,
        }
    }
}

/// Owns conversation compaction and contributes the rolling summary to the
/// history channel. See the module docs.
pub struct RollingSummaryContextAdapter<F: ClawFs + 'static> {
    /// A clone of the agent's transcript store (shares the same `Arc`-backed state
    /// the transcript writes to): read for committed turns. Never mutated here.
    store: TranscriptStore<F>,
    /// How aged windows are turned into a summary.
    compactor: Arc<dyn Compactor>,
    /// When/what to compact.
    policy: CompactionPolicy,
    /// Shared boundary: advanced here as turns are summarized, read by the recent
    /// adapter so it stops rendering them verbatim.
    cursor: SummaryCursor,
    /// Accumulated summary segments (each a segment's messages), ascending by
    /// coverage. Only the tick thread touches this, so it needs no lock. The
    /// shared cursor — not these — tracks how far coverage reaches.
    segments: Vec<Vec<Value>>,
    /// Cached summary snapshot (all segments flattened), rebuilt when a new
    /// segment is produced, then emitted into the context sink.
    cached: Arc<Value>,
}

impl<F: ClawFs + 'static> RollingSummaryContextAdapter<F> {
    /// Build the adapter over a clone of the agent's transcript `store`, the
    /// `compactor`, the compaction `policy`, and the shared `cursor` the
    /// recent-tail adapter reads.
    pub fn new(
        store: TranscriptStore<F>,
        compactor: Arc<dyn Compactor>,
        policy: CompactionPolicy,
        cursor: SummaryCursor,
    ) -> Self {
        Self {
            store,
            compactor,
            policy,
            cursor,
            segments: Vec::new(),
            cached: Arc::new(Value::Array(Vec::new())),
        }
    }

    /// Select the next window of aged committed turns to summarize and run the
    /// [`Compactor`] on the local runtime when the verbatim history has outgrown
    /// the trigger budget.
    ///
    /// Pull, not push: called from [`contribute`](ContextAdapter::contribute) on
    /// the tick thread, so the cursor is current and the same turns are never
    /// selected twice.
    async fn maybe_schedule_compaction(&mut self) {
        let Some((id_end, window_messages)) = self.select_window() else {
            return;
        };

        match self.compactor.compact(&window_messages).await {
            Ok(messages) => {
                self.cursor.advance_to(id_end);
                self.segments.push(messages);
                let flat: Vec<Value> = self
                    .segments
                    .iter()
                    .flat_map(|segment| segment.iter().cloned())
                    .collect();
                self.cached = Arc::new(Value::Array(flat));
            }
            Err(error) => tracing::warn!(%error, "conversation compaction skipped"),
        }
    }

    /// Pick the oldest aged turns to summarize next, or `None` when there is
    /// nothing to do.
    ///
    /// Walks the committed turns past the cursor (the current verbatim history),
    /// returns `None` until their token estimate crosses `trigger_tokens`, then
    /// keeps the newest `keep_recent_tokens` verbatim and packs the oldest of the
    /// rest into a chunk of up to `segment_token_budget` tokens. Returns the chunk
    /// plus the highest turn id it covers.
    fn select_window(&self) -> Option<(TurnId, Vec<Value>)> {
        let turns = self.store.turns_snapshot();
        let cursor = self.cursor.covered_through();
        let uncovered: Vec<&Turn> = turns.iter().filter(|turn| turn.id.0 > cursor).collect();

        let uncovered_tokens: usize = uncovered
            .iter()
            .flat_map(|turn| turn.messages.iter())
            .map(estimate_message_tokens)
            .sum();
        if uncovered_tokens <= self.policy.trigger_tokens {
            return None; // small enough to keep entirely verbatim
        }

        let verbatim_count = recent_tail_count(&uncovered, self.policy.keep_recent_tokens);
        let aged = uncovered.get(..uncovered.len().saturating_sub(verbatim_count))?;
        let first = aged.first()?;

        let mut window_messages: Vec<Value> = Vec::new();
        let mut id_end = first.id;
        let mut tokens = 0usize;
        for turn in aged {
            let turn_tokens: usize = turn.messages.iter().map(estimate_message_tokens).sum();
            if !window_messages.is_empty()
                && tokens.saturating_add(turn_tokens) > self.policy.segment_token_budget
            {
                break;
            }
            window_messages.extend(turn.messages.iter().cloned());
            id_end = turn.id;
            tokens = tokens.saturating_add(turn_tokens);
        }

        if window_messages.is_empty() {
            return None;
        }
        Some((id_end, window_messages))
    }
}

impl<F: ClawFs + 'static> ContextAdapter for RollingSummaryContextAdapter<F> {
    fn id(&self) -> &str {
        ADAPTER_ID
    }

    fn prepare<'a>(&'a mut self, _input: ContextAdapterInput<'a>) -> ContextAdapterFuture<'a> {
        Box::pin(async move {
            self.maybe_schedule_compaction().await;
        })
    }

    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        if let Some(items) = self.cached.as_array() {
            for message in items {
                output.message(BlockKind::ConversationSummary, message);
            }
        }
    }
}

/// How many of the newest `turns` form the verbatim tail under `keep_recent_tokens`.
///
/// Accumulates tokens newest-first, stopping when the budget is met. Always
/// returns at least 1 (when `turns` is non-empty) so there is always something
/// verbatim to give the model.
fn recent_tail_count(turns: &[&Turn], keep_recent_tokens: usize) -> usize {
    if turns.is_empty() {
        return 0;
    }
    let mut tokens = 0usize;
    let mut count = 0usize;
    for turn in turns.iter().rev() {
        let turn_tokens: usize = turn.messages.iter().map(estimate_message_tokens).sum();
        tokens = tokens.saturating_add(turn_tokens);
        count = count.saturating_add(1);
        if tokens >= keep_recent_tokens {
            break;
        }
    }
    count.max(1)
}

// todo: replace this byte-length heuristic with a real tokenizer estimate that
// matches the active backend's accounting. It only needs to be monotonic and
// roughly proportional for the compaction trigger to behave.
fn estimate_message_tokens(message: &Value) -> usize {
    message.to_string().len() / CHARS_PER_TOKEN + 1
}
