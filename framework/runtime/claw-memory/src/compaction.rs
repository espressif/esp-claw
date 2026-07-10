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
use std::error::Error;
use std::future::Future;
use std::pin::Pin;
use strum::IntoStaticStr;

/// Future returned by [`Compactor::compact`].
pub type CompactFuture<'a> = Pin<Box<dyn Future<Output = Result<Vec<Value>, CompactError>> + 'a>>;

/// Failure from a [`Compactor`].
///
/// Compaction is best-effort: on error the tape keeps the un-compacted groups
/// and tries again later, but concrete backend failures still keep their source
/// chain for diagnostics.
#[derive(Debug, IntoStaticStr, thiserror::Error)]
pub enum CompactError {
    /// The summarization backend (e.g. the LLM client) failed.
    #[strum(serialize = "backend")]
    #[error("compaction backend failed")]
    Backend(#[from] CompactBackendError),
    /// The backend returned no summary text.
    #[strum(serialize = "empty_summary")]
    #[error("compaction backend returned an empty summary")]
    EmptySummary,
}

/// Backend failure captured by the backend-agnostic [`Compactor`] seam.
///
/// `claw-memory` deliberately has no dependency on any LLM crate, so concrete
/// compactor implementations wrap their native error here instead of flattening
/// it into a string.
#[derive(Debug, thiserror::Error)]
#[error(transparent)]
pub struct CompactBackendError {
    #[from]
    source: Box<dyn Error + Send + Sync + 'static>,
}

impl CompactBackendError {
    /// Preserve a concrete backend error behind the backend-agnostic seam.
    pub fn new(error: impl Error + Send + Sync + 'static) -> Self {
        Self {
            source: Box::new(error),
        }
    }
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
/// use futures_lite::future::block_on;
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

#[cfg(test)]
mod tests {
    use super::{CompactBackendError, CompactError};

    #[test]
    fn compact_error_converts_variants_to_stable_trace_kinds() {
        let backend = CompactError::Backend(CompactBackendError::new(std::fmt::Error));
        let empty_summary = CompactError::EmptySummary;

        let backend_kind: &'static str = (&backend).into();
        let empty_summary_kind: &'static str = (&empty_summary).into();

        assert_eq!(backend_kind, "backend");
        assert_eq!(empty_summary_kind, "empty_summary");
    }
}
