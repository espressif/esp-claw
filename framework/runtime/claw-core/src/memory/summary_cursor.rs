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
//! The cursor is a single, explicit coordination channel both adapters hold a
//! clone of: the summary adapter writes it (monotonically), the recent adapter
//! reads it. The transcript store itself never sees it — compaction is entirely
//! an agent-layer, request-assembly concern.

use std::cell::Cell;
use std::rc::Rc;

use claw_memory::TurnId;

/// The highest committed [`TurnId`] the rolling summary has covered, shared
/// between the summary and recent-tail adapters. `None` means nothing is
/// summarized yet, so the whole transcript renders verbatim.
///
/// Cheap to clone (an `Rc` bump); both adapters hold the same underlying value.
/// This is intentionally local to the agent's context assembly path, not a
/// cross-thread coordination primitive.
#[derive(Clone, Default)]
pub(crate) struct SummaryCursor(Rc<Cell<Option<TurnId>>>);

impl SummaryCursor {
    /// A fresh cursor with no summarized turns.
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// The highest committed turn id summarized so far.
    ///
    /// The recent-tail adapter renders committed turns whose id is strictly
    /// greater than this.
    pub(crate) fn covered_through(&self) -> Option<TurnId> {
        self.0.get()
    }

    /// Advance the cursor to cover through `id`, monotonically.
    ///
    /// Uses a max update so an out-of-order or stale update can never move the
    /// boundary backwards (which would re-expose already-summarized turns as
    /// verbatim, duplicating them against the summary).
    pub(crate) fn advance_to(&self, id: TurnId) {
        self.0
            .set(Some(self.0.get().map_or(id, |current| current.max(id))));
    }
}
