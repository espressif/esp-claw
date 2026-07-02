//! The extraction seam: distilling durable facts from a conversation transcript.
//!
//! Extraction is "read recent conversation, decide what is worth remembering
//! long-term, and produce a few concise facts". *How* that is done (an LLM call,
//! a heuristic, nothing) is a policy injected as an [`Extractor`], mirroring how
//! [`Compactor`](claw_memory::Compactor) is injected into the conversation tape.
//! The long-term memory adapter owns the *mechanism* (when to extract, routing
//! the results to a tier, persisting); the `Extractor` owns only the
//! *transformation*, so it stays free of any storage concern.

use super::tier::MemoryTier;
use core::future::Future;
use core::pin::Pin;

/// One fact an [`Extractor`] distilled from a transcript.
///
/// Carries the same shape a [`MemoryDraft`](claw_memory::MemoryDraft) needs, plus
/// an optional [`tier`](Self::tier) hint the
/// [`TierClassifier`](crate::memory::TierClassifier) may honor. The adapter
/// converts this into a draft and routes it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ExtractedItem {
    /// The distilled fact, in concise third person.
    pub content: String,
    /// Topic labels to file it under.
    pub tags: Vec<String>,
    /// Extra search terms.
    pub keywords: Vec<String>,
    /// Optional routing hint; `None` lets the classifier decide.
    pub tier: Option<MemoryTier>,
}

/// Failure from an [`Extractor`].
///
/// Extraction is best-effort: on error the adapter logs the reason and keeps the
/// existing memory, so a displayable string is all a caller needs.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ExtractError {
    /// The extraction backend (e.g. the LLM client) failed.
    #[error("extraction backend failed: {0}")]
    Backend(String),
}

pub type ExtractFuture<'a> =
    Pin<Box<dyn Future<Output = Result<Vec<ExtractedItem>, ExtractError>> + 'a>>;

/// Turns a conversation transcript into zero or more durable facts.
///
/// `transcript` is a flattened, self-contained snapshot of the recent
/// conversation. Returning an empty `Vec` is normal — most turns hold nothing
/// worth remembering.
pub trait Extractor {
    /// Extract durable facts from `transcript`.
    fn extract<'a>(&'a self, transcript: &'a str) -> ExtractFuture<'a>;
}

/// An [`Extractor`] that never extracts: every call yields no facts.
///
/// For wiring where extraction is undesired or irrelevant — host CLIs that keep
/// only the transcript, and tests that need a memory adapter without an LLM.
#[derive(Debug, Clone, Copy, Default)]
pub struct NoopExtractor;

impl Extractor for NoopExtractor {
    fn extract<'a>(&'a self, _transcript: &'a str) -> ExtractFuture<'a> {
        Box::pin(async { Ok(Vec::new()) })
    }
}
