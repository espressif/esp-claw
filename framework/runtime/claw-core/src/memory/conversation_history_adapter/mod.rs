//! Conversation-history projection and compaction for one agent.
//!
//! [`ConversationHistoryContextAdapter`] owns both sides of the request-time
//! history boundary: summary messages for the compacted prefix and a cached
//! verbatim tail for everything after that prefix. Keeping the transcript,
//! coverage cursor, summary, and tail cache in one object makes one
//! [`ContextAdapter::prepare`]/[`ContextAdapter::contribute`] cycle an atomic
//! projection: every committed turn is represented exactly once, while the open
//! turn always remains verbatim.

mod llm_compactor;

use claw_context::{BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{Compactor, TranscriptStore, Turn, TurnId};
use serde_json::Value;
use tracing::Instrument as _;

use crate::memory::traits::{ContextAdapter, ContextAdapterFuture, History};

pub(crate) use llm_compactor::LlmCompactor;

/// Rough bytes-per-token divisor for the size estimate. See
/// [`estimate_message_tokens`].
const CHARS_PER_TOKEN: usize = 4;

/// The conversation-compaction policy knobs the adapter applies.
#[derive(Clone, Copy, Debug)]
pub(crate) struct CompactionPolicy {
    /// Start compacting once the verbatim history past the cursor exceeds this.
    trigger_tokens: usize,
    /// Token budget for the verbatim tail kept out of every summary.
    keep_recent_tokens: usize,
    /// Max tokens summarized per compaction pass.
    segment_token_budget: usize,
}

impl CompactionPolicy {
    pub(crate) fn new(
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

/// Owns the complete request-time projection of one conversation transcript.
pub(crate) struct ConversationHistoryContextAdapter<F: ClawFs + 'static> {
    /// Shared transcript source of truth. This adapter only reads it.
    transcript: TranscriptStore<F>,
    /// Transformation used to summarize one aged prefix window.
    compactor: Box<dyn Compactor>,
    policy: CompactionPolicy,
    /// Highest committed turn represented by `summary_messages`.
    covered_through: Option<TurnId>,
    /// Non-overlapping summaries of committed transcript prefixes.
    summary_messages: Vec<Value>,
    /// Verbatim committed turns after `covered_through`, plus the open turn.
    verbatim_tail: Vec<Value>,
    /// Transcript version represented by `verbatim_tail`.
    cached_version: u64,
    /// Coverage boundary represented by `verbatim_tail`.
    cached_covered_through: Option<TurnId>,
    primed: bool,
}

impl<F: ClawFs + 'static> ConversationHistoryContextAdapter<F> {
    pub(crate) fn new(
        transcript: TranscriptStore<F>,
        compactor: Box<dyn Compactor>,
        policy: CompactionPolicy,
    ) -> Self {
        Self {
            transcript,
            compactor,
            policy,
            covered_through: None,
            summary_messages: Vec::new(),
            verbatim_tail: Vec::new(),
            cached_version: 0,
            cached_covered_through: None,
            primed: false,
        }
    }

    /// Compact at most one aged prefix, then cache the exact complementary tail.
    async fn prepare_projection(&mut self) {
        if let Some((id_end, window_messages, estimated_tokens)) = self.select_window() {
            let span = tracing::info_span!(
                "context.compact",
                message_count = window_messages.len() as u64,
                estimated_tokens = estimated_tokens as u64,
            );
            let result = self
                .compactor
                .compact(&window_messages)
                .instrument(span.clone())
                .await;
            match result {
                Ok(messages) => {
                    span.in_scope(|| {
                        tracing::info!(name: "completed", summary_count = messages.len() as u64);
                    });
                    self.covered_through = Some(
                        self.covered_through
                            .map_or(id_end, |current| current.max(id_end)),
                    );
                    self.summary_messages.extend(messages);
                }
                Err(error) => {
                    let kind: &'static str = (&error).into();
                    span.in_scope(|| tracing::warn!(name: "failed", kind));
                }
            }
        }

        // Refresh only after a successful boundary advance (or failed/no-op
        // compaction), so the cached tail always complements the summary state
        // this same object will contribute.
        self.refresh_verbatim_tail();
    }

    fn refresh_verbatim_tail(&mut self) {
        let version = self.transcript.version();
        if self.primed
            && version == self.cached_version
            && self.covered_through == self.cached_covered_through
        {
            return;
        }

        self.verbatim_tail.clear();
        let turns = self.transcript.turns_snapshot();
        for turn in turns
            .iter()
            .filter(|turn| self.covered_through.is_none_or(|covered| turn.id > covered))
        {
            self.verbatim_tail.extend(turn.messages.iter().cloned());
        }
        self.verbatim_tail
            .extend(self.transcript.open_turn_messages());
        self.cached_version = version;
        self.cached_covered_through = self.covered_through;
        self.primed = true;
    }

    /// Pick the oldest uncovered committed turns eligible for the next summary.
    fn select_window(&self) -> Option<(TurnId, Vec<Value>, usize)> {
        let turns = self.transcript.turns_snapshot();
        let uncovered: Vec<&Turn> = turns
            .iter()
            .filter(|turn| self.covered_through.is_none_or(|covered| turn.id > covered))
            .collect();

        let uncovered_tokens: usize = uncovered
            .iter()
            .flat_map(|turn| turn.messages.iter())
            .map(estimate_message_tokens)
            .sum();
        if uncovered_tokens <= self.policy.trigger_tokens {
            return None;
        }

        let verbatim_count = recent_tail_count(&uncovered, self.policy.keep_recent_tokens);
        let aged = uncovered.get(..uncovered.len().saturating_sub(verbatim_count))?;
        let first = aged.first()?;

        let mut window_messages = Vec::new();
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

        (!window_messages.is_empty()).then_some((id_end, window_messages, tokens))
    }
}

impl<F: ClawFs + 'static> ContextAdapter for ConversationHistoryContextAdapter<F> {
    fn prepare<'a>(&'a mut self, _history: &'a dyn History) -> ContextAdapterFuture<'a> {
        Box::pin(async move {
            self.prepare_projection().await;
        })
    }

    fn contribute(&mut self, output: &mut ContextSink<'_>) {
        // The production lifecycle always prepares first. Keeping this guarded
        // refresh here also makes a direct first contribution complete and
        // preserves coherence if the transcript changes between the two calls.
        self.refresh_verbatim_tail();
        for message in &self.summary_messages {
            output.message(BlockKind::ConversationSummary, message);
        }
        for message in &self.verbatim_tail {
            output.message(BlockKind::RecentContext, message);
        }
    }
}

/// How many of the newest `turns` form the verbatim tail under the token budget.
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

// todo: replace this byte-length heuristic with a tokenizer estimate matching
// the active backend. It only needs to remain monotonic for trigger behavior.
fn estimate_message_tokens(message: &Value) -> usize {
    message.to_string().len() / CHARS_PER_TOKEN + 1
}

#[cfg(test)]
mod tests {
    use claw_context::Context;
    use claw_interface::MemFs;
    use claw_memory::{CompactFuture, Compactor, TranscriptStore};
    use futures_lite::future::block_on;
    use serde_json::{json, Value};

    use super::{CompactionPolicy, ContextAdapter, ConversationHistoryContextAdapter, History};

    struct WindowEchoCompactor;

    impl Compactor for WindowEchoCompactor {
        fn compact<'a>(&'a self, window: &'a [Value]) -> CompactFuture<'a> {
            Box::pin(async move {
                let covered = window
                    .iter()
                    .filter_map(|message| message.get("content").and_then(Value::as_str))
                    .collect::<Vec<_>>()
                    .join("|");
                Ok(vec![json!({
                    "role": "system",
                    "content": format!("summary:{covered}"),
                })])
            })
        }
    }

    #[test]
    fn one_projection_has_summary_prefix_and_exact_complementary_tail() {
        let transcript = TranscriptStore::<MemFs>::in_memory(1);
        for text in ["turn-one", "turn-two", "turn-three"] {
            transcript.push_user_message(text);
            transcript.commit_open_turn();
        }
        transcript.push_user_message("open-four");
        let expected_covered_through = transcript.turns_snapshot()[1].id;

        let mut adapter = ConversationHistoryContextAdapter::new(
            transcript.clone(),
            Box::new(WindowEchoCompactor),
            CompactionPolicy::new(0, 1, usize::MAX),
        );
        block_on(adapter.prepare(&transcript as &dyn History));

        let mut context = Context::new();
        let mut sink = context.sink();
        adapter.contribute(&mut sink);
        let rendered = sink.into_history();

        assert_eq!(adapter.covered_through, Some(expected_covered_through));
        assert_eq!(
            rendered,
            json!([
                {"role": "system", "content": "summary:turn-one|turn-two"},
                {"role": "user", "content": "turn-three"},
                {"role": "user", "content": "open-four"},
            ])
        );
        let rendered_text = rendered.to_string();
        assert_eq!(rendered_text.matches("turn-one").count(), 1);
        assert_eq!(rendered_text.matches("turn-two").count(), 1);
        assert_eq!(rendered_text.matches("turn-three").count(), 1);
        assert_eq!(rendered_text.matches("open-four").count(), 1);
    }
}
