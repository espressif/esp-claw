//! [`SummaryCursor`] — the explicit boundary shared between the two conversation
//! context adapters.
//!
//! Compaction splits the transcript into two halves rendered by two sibling
//! adapters:
//!
//! - [`RollingSummaryContextAdapter`](super::RollingSummaryContextAdapter) owns
//!   the summary of the **older** turns and advances this cursor to the highest
//!   [`TurnId`](claw_memory::TurnId) it has summarized.
//! - [`RecentMessagesContextAdapter`](super::RecentMessagesContextAdapter) renders
//!   the **verbatim tail**: the committed turns *after* this cursor, plus the open
//!   turn.
//!
//! Because summarization runs asynchronously (on the shared pool), "how far the
//! summary has advanced" is a live progress value, not something either adapter
//! can derive independently — render against a stale boundary and a turn would
//! fall into neither half (a gap). So the cursor is a single, explicit,
//! shared coordination channel both adapters hold a clone of: the summary
//! adapter writes it (monotonically), the recent adapter reads it. The transcript
//! store itself never sees it — compaction is entirely an agent-layer,
//! request-assembly concern.

use std::sync::{Arc, Mutex};

use claw_memory::TurnId;

/// The highest committed [`TurnId`] the rolling summary has covered, shared
/// between the summary and recent-tail adapters. `0` means nothing is summarized
/// yet (turn ids are 1-based), so the whole transcript renders verbatim.
///
/// Cheap to clone (an `Arc` bump); both adapters hold the same underlying value.
#[derive(Clone, Default)]
pub struct SummaryCursor(Arc<Mutex<u64>>);

impl SummaryCursor {
    /// A fresh cursor at `0` — nothing summarized yet.
    pub fn new() -> Self {
        Self::default()
    }

    /// The highest committed turn id summarized so far (`0` = none).
    ///
    /// The recent-tail adapter renders committed turns whose id is strictly
    /// greater than this.
    pub fn covered_through(&self) -> u64 {
        *self
            .0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Advance the cursor to cover through `id`, monotonically.
    ///
    /// Uses a max update so an out-of-order or stale update can never move the
    /// boundary backwards (which would re-expose already-summarized turns as
    /// verbatim, duplicating them against the summary).
    pub fn advance_to(&self, id: TurnId) {
        let mut covered = self
            .0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        *covered = (*covered).max(id.0);
    }
}
