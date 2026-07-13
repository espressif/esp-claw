//! The extraction seam: reconciling durable memory against a conversation.
//!
//! Extraction is "read the recent conversation *and the current memory*, decide
//! what should change, and emit a few [`MemoryOp`]s". *How* that is done (an LLM
//! call, a heuristic, nothing) is a policy injected as an [`Extractor`],
//! mirroring how [`Compactor`](claw_memory::Compactor) is injected into the
//! conversation tape. The long-term memory adapter owns the *mechanism* (when to
//! extract, routing new facts to a tier, applying edits/removals, persisting);
//! the `Extractor` owns only the *transformation*, so it stays free of any
//! storage concern.
//!
//! Giving the extractor the current memory (as [`MemorySnapshot`]s carrying each
//! item's id) is what lets it go beyond appending: it can reference an existing
//! id to [`Replace`](MemoryOp::Replace) a stale fact or [`Forget`](MemoryOp::Forget)
//! one the user retracted, instead of only ever adding.

use claw_api::ChatError;
use claw_memory::MemoryId;
use core::future::Future;
use core::pin::Pin;
use strum::IntoStaticStr;

/// One fact an [`Extractor`] distilled from a transcript.
///
/// Carries the same shape a [`MemoryDraft`](claw_memory::MemoryDraft) needs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ExtractedItem {
    /// The distilled fact, in concise third person.
    pub(super) content: String,
    /// Topic labels to file it under.
    pub(super) tags: Vec<String>,
    /// Extra search terms.
    pub(super) keywords: Vec<String>,
}

/// A compact view of one already-stored fact, handed to the [`Extractor`] so it
/// can reference the fact's [`id`](Self::id) when proposing an edit or removal.
///
/// Deliberately smaller than [`MemoryItem`](claw_memory::MemoryItem): the
/// extractor only needs to recognize a fact and cite it, not its provenance or
/// ordering.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct MemorySnapshot {
    /// The stored fact's stable id (the handle for `Replace`/`Forget`).
    pub(super) id: MemoryId,
    /// The stored fact's content.
    pub(super) content: String,
    /// The labels it is filed under.
    pub(super) tags: Vec<String>,
}

/// What the [`Extractor`] sees: the recent conversation plus the current memory.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ExtractionInput<'a> {
    /// A flattened, self-contained snapshot of the recent conversation.
    pub(super) transcript: &'a str,
    /// The facts already stored, so edits/removals can cite an existing id.
    pub(super) existing: &'a [MemorySnapshot],
}

/// A single change an [`Extractor`] proposes against long-term memory.
///
/// The adapter applies each op: `Add` stores a new fact, `Replace` edits the
/// cited fact in place, `Forget` removes it. `Replace`/`Forget` name a fact by
/// the [`MemoryId`] the extractor saw in a [`MemorySnapshot`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum MemoryOp {
    /// Store a newly-distilled fact.
    Add(ExtractedItem),
    /// Replace the cited fact's content/labels with `item`.
    Replace {
        /// The existing fact to edit.
        id: MemoryId,
        /// Its new content/labels.
        item: ExtractedItem,
    },
    /// Remove the cited fact (the user retracted or superseded it).
    Forget {
        /// The existing fact to drop.
        id: MemoryId,
    },
}

/// Failure from an [`Extractor`].
///
/// Extraction is best-effort: on error the adapter logs the reason and keeps the
/// existing memory, but the concrete source is still preserved for diagnostics.
#[derive(Debug, Clone, IntoStaticStr, PartialEq, Eq, thiserror::Error)]
pub(crate) enum ExtractError {
    /// The extraction backend (e.g. the LLM client) failed.
    #[strum(serialize = "backend")]
    #[error("extraction backend failed: {0}")]
    Backend(#[from] ChatError),
    /// The extraction backend produced no usable text.
    #[strum(serialize = "empty_output")]
    #[error("extraction backend returned empty output")]
    EmptyOutput,
}

pub(super) type ExtractFuture<'a> =
    Pin<Box<dyn Future<Output = Result<Vec<MemoryOp>, ExtractError>> + 'a>>;

/// Reconciles long-term memory against a conversation, emitting zero or more
/// [`MemoryOp`]s.
///
/// Returning an empty `Vec` is normal — most turns hold nothing worth changing.
pub(crate) trait Extractor {
    /// Propose memory changes from `input` (transcript + current memory).
    fn extract<'a>(&'a self, input: ExtractionInput<'a>) -> ExtractFuture<'a>;
}
