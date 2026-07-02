//! The compaction seam.
//!
//! Compaction is "fold an aged window of conversation messages into a shorter
//! summary so the working context stays inside the model's budget". *How* that
//! summary is produced (an LLM summarization call, a cheap heuristic, …) is a
//! transformation, injected as a [`Compactor`].
//!
//! Compaction is **not** a storage concern: the [`TranscriptStore`](crate::TranscriptStore)
//! keeps the full verbatim record and knows nothing of summaries. The *policy*
//! (when to compact, which window, where the summary goes) lives in the agent
//! layer's rolling-summary context adapter, which drives a `Compactor`. This seam
//! is defined here only so it stays free of any LLM dependency, exactly like the
//! crate depends on the `ClawFs` trait and never on its implementation.

use serde_json::Value;
use std::future::Future;
use std::pin::Pin;

/// Future returned by [`Compactor::compact`].
pub type CompactFuture<'a> = Pin<Box<dyn Future<Output = Result<Vec<Value>, CompactError>> + 'a>>;

/// Failure from a [`Compactor`].
///
/// Compaction is best-effort: on error the tape keeps the un-compacted groups
/// and tries again later, so the only thing a caller needs is a displayable
/// reason for logs.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum CompactError {
    /// The summarization backend (e.g. the LLM client) failed; carries a
    /// displayable reason for logging.
    #[error("compaction backend failed: {0}")]
    Backend(String),
}

/// Turns one chunk of aged chat messages into a compact summary.
///
/// `window` is a self-contained snapshot (already detached from the tape's
/// lock) of the oldest groups being retired in this round — bounded by the
/// tape's `segment_token_budget`. It contains only raw messages; the tape does
/// **not** fold a previous summary in, because each chunk becomes its own
/// independent, non-overlapping compact segment covering a distinct id range.
///
/// The returned messages *are* that segment, so an implementation is free to
/// fold the chunk into a single message or a small handful. Returning an empty
/// `Vec` is allowed but produces an empty segment; prefer a concise summary.
///
/// # Examples
///
/// A trivial compactor that records how many messages it folded:
///
/// ```
/// use claw_memory::{CompactError, Compactor};
/// use claw_utils::block_on;
/// use serde_json::{json, Value};
///
/// struct CountingCompactor;
///
/// impl Compactor for CountingCompactor {
///     fn compact<'a>(&'a self, window: &'a [Value]) -> claw_memory::CompactFuture<'a> {
///         Box::pin(async move { Ok(vec![json!({
///             "role": "system",
///             "content": format!("summary of {} earlier messages", window.len()),
///         })]) })
///     }
/// }
///
/// let summary = block_on(CountingCompactor
///     .compact(&[json!({ "role": "user", "content": "hi" })]))?;
/// assert_eq!(summary[0]["content"], "summary of 1 earlier messages");
/// # Ok::<(), CompactError>(())
/// ```
pub trait Compactor {
    /// Summarize one chunk of aged messages into the messages of a single
    /// compact segment.
    fn compact<'a>(&'a self, window: &'a [Value]) -> CompactFuture<'a>;
}

/// A [`Compactor`] that never compacts: every call yields an empty segment.
///
/// For wiring where summarization is undesired or irrelevant — host CLIs that
/// keep the full transcript, and tests that need a memory without an LLM. Behind
/// the `compactor-stub` feature so it is never built into firmware unless
/// explicitly opted in.
#[cfg(feature = "compactor-stub")]
#[derive(Debug, Clone, Copy, Default)]
pub struct NoopCompactor;

#[cfg(feature = "compactor-stub")]
impl Compactor for NoopCompactor {
    fn compact<'a>(&'a self, _window: &'a [Value]) -> CompactFuture<'a> {
        Box::pin(async { Ok(Vec::new()) })
    }
}
