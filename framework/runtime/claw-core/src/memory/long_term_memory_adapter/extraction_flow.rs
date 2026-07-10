use claw_interface::ClawFs;
use claw_memory::{MemoryDraft, MemoryPatch};
use serde_json::Value;
use tracing::Instrument as _;

use crate::memory::traits::History;

use super::{ExtractionInput, LongTermMemoryContextAdapter, MemoryOp};

/// Extraction throttle: after the first extraction, the transcript version must
/// advance by at least this much before the next extraction runs.
const EXTRACT_MIN_VERSION_DELTA: u64 = 8;

impl<F: ClawFs + 'static> LongTermMemoryContextAdapter<F> {
    /// Run extraction when the transcript has advanced.
    ///
    /// Pull, not push: called from `prepare` on the tick thread, it self-detects
    /// new conversation via the transcript version. Store dedup absorbs facts
    /// re-extracted across turns.
    pub(super) async fn maybe_schedule_extraction(&mut self, history: &dyn History) {
        let version = history.version();
        if version == self.extract_cursor {
            return; // transcript unchanged since the last extraction
        }
        if self.extract_cursor != 0
            && version.saturating_sub(self.extract_cursor) < EXTRACT_MIN_VERSION_DELTA
        {
            return;
        }

        let snapshot = history.messages();
        let transcript = flatten_transcript(&snapshot);
        if transcript.trim().is_empty() {
            return;
        }
        let version_delta = version.saturating_sub(self.extract_cursor);
        self.extract_cursor = version;
        let existing = self.stores.snapshot();
        let input = ExtractionInput {
            transcript: &transcript,
            existing: &existing,
        };
        let span = tracing::info_span!(
            "context.extract",
            transcript_version = version,
            version_delta,
            transcript_bytes = transcript.len() as u64,
            existing_count = existing.len() as u64,
        );
        let result = self.extractor.extract(input).instrument(span.clone()).await;
        match result {
            Ok(ops) => {
                let mut add_count = 0u64;
                let mut replace_count = 0u64;
                let mut forget_count = 0u64;
                for op in &ops {
                    match op {
                        MemoryOp::Add(_) => add_count = add_count.saturating_add(1),
                        MemoryOp::Replace { .. } => {
                            replace_count = replace_count.saturating_add(1);
                        }
                        MemoryOp::Forget { .. } => {
                            forget_count = forget_count.saturating_add(1);
                        }
                    }
                }
                span.in_scope(|| {
                    tracing::info!(
                        name: "completed",
                        operation_count = ops.len() as u64,
                        add_count,
                        replace_count,
                        forget_count,
                    );
                });
                for op in ops {
                    self.apply_op(op);
                }
            }
            Err(error) => {
                let kind: &'static str = (&error).into();
                span.in_scope(|| tracing::warn!(name: "failed", kind));
            }
        }
    }

    fn apply_op(&self, op: MemoryOp) {
        match op {
            MemoryOp::Add(item) => {
                let draft = MemoryDraft::new(item.content)
                    .with_tags(item.tags)
                    .with_keywords(item.keywords)
                    .with_source("extracted");
                self.stores.store(draft);
            }
            MemoryOp::Replace { id, item } => {
                let patch = MemoryPatch {
                    content: Some(item.content),
                    tags: Some(item.tags),
                    keywords: Some(item.keywords),
                };
                let _ = self.stores.update(&id, patch);
            }
            MemoryOp::Forget { id } => {
                let _ = self.stores.forget(&id);
            }
        }
    }
}

/// Flatten a transcript snapshot (a JSON array of chat messages) into the
/// role-prefixed plain text an extractor reads. Messages without string content
/// (e.g. an assistant turn carrying only `tool_calls`) are skipped.
fn flatten_transcript(messages: &Value) -> String {
    let Some(items) = messages.as_array() else {
        return String::new();
    };
    let mut out = String::new();
    for message in items {
        let Some(role) = message.get("role").and_then(Value::as_str) else {
            continue;
        };
        let Some(content) = message.get("content").and_then(Value::as_str) else {
            continue;
        };
        if role.is_empty() || content.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push('\n');
        }
        out.push_str(role);
        out.push_str(": ");
        out.push_str(content);
    }
    out
}
