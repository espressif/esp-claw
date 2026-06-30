//! The soft-hide "retry then fail" block policy: a small mutable counter that
//! tracks consecutive gating-blocked tool rounds and decides when the agent has
//! given the model enough chances to self-correct.
//!
//! This is **conversation state**, deliberately kept out of [`ToolSet`](crate::ToolSet)
//! (which owns the tool catalog and the once-rendered, cached wire surfaces). The
//! agent owns one [`BlockPolicy`] and feeds each tool round's blocked calls to
//! [`record_round`](BlockPolicy::record_round).

/// Consecutive gating-blocked tool rounds tolerated before
/// [`record_round`](BlockPolicy::record_round) reports [`ToolBlockVerdict::Exhausted`].
/// One round buys the model a single self-correction nudge.
pub const DEFAULT_BLOCK_RETRIES: u32 = 1;

/// The verdict [`BlockPolicy::record_round`] returns after a tool round, driving
/// the soft-hide "retry then fail" policy.
///
/// The policy counts consecutive rounds with a gating-blocked call (the model
/// already received a tool error to self-correct from); once that streak exceeds
/// the tolerated budget the round is [`Exhausted`](Self::Exhausted) and the
/// caller (the agent) should end the task. A clean round resets the streak and
/// yields [`Continue`](Self::Continue).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ToolBlockVerdict {
    /// Keep iterating: the round was clean, or the blocked streak is still within
    /// the retry budget.
    Continue,
    /// The blocked streak exceeded the budget. `name` is one offending tool, for
    /// the caller's failure report.
    Exhausted {
        /// A tool that was blocked this round.
        name: String,
    },
}

/// The soft-hide "retry then fail" streak counter.
///
/// Holds the tolerated budget plus the live count of consecutive blocked rounds.
/// A clean round resets the streak; a blocked round bumps it and, once it exceeds
/// the budget, yields [`ToolBlockVerdict::Exhausted`]. Defaults to
/// [`DEFAULT_BLOCK_RETRIES`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BlockPolicy {
    /// How many consecutive gating-blocked rounds to tolerate before reporting
    /// [`ToolBlockVerdict::Exhausted`]. `0` fails on the first blocked round.
    retries: u32,
    /// Count of consecutive rounds that had at least one gating-blocked call.
    /// Reset to 0 by any clean round; compared against `retries`.
    consecutive_blocks: u32,
}

impl Default for BlockPolicy {
    fn default() -> Self {
        Self::new(DEFAULT_BLOCK_RETRIES)
    }
}

impl BlockPolicy {
    /// A policy that tolerates `retries` consecutive blocked rounds before
    /// exhausting. `0` fails on the first blocked round.
    pub fn new(retries: u32) -> Self {
        Self {
            retries,
            consecutive_blocks: 0,
        }
    }

    /// The configured tolerance (consecutive blocked rounds before exhaustion).
    pub fn retries(&self) -> u32 {
        self.retries
    }

    /// Account for one completed tool round and report whether soft-hide gating
    /// has now blocked too many rounds in a row.
    ///
    /// `blocked` is the names of the calls the round refused via soft-hide (empty
    /// for a clean round). A clean round resets the streak; a blocked round bumps
    /// it and, once it exceeds the budget, returns
    /// [`Exhausted`](ToolBlockVerdict::Exhausted) naming one offender so the agent
    /// can end the task. The model already received a tool error for each blocked
    /// call, so an `Exhausted` verdict means it ignored the restriction past the
    /// budget.
    pub fn record_round(&mut self, blocked: &[&str]) -> ToolBlockVerdict {
        let Some(first) = blocked.first() else {
            self.consecutive_blocks = 0;
            return ToolBlockVerdict::Continue;
        };
        self.consecutive_blocks = self.consecutive_blocks.saturating_add(1);
        if self.consecutive_blocks > self.retries {
            ToolBlockVerdict::Exhausted {
                name: (*first).to_string(),
            }
        } else {
            ToolBlockVerdict::Continue
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clean_round_keeps_continuing_and_resets_the_streak() {
        let mut policy = BlockPolicy::default();
        assert_eq!(policy.record_round(&["read"]), ToolBlockVerdict::Continue);
        // A clean round clears the streak so the next block starts over.
        assert_eq!(policy.record_round(&[]), ToolBlockVerdict::Continue);
        assert_eq!(policy.record_round(&["read"]), ToolBlockVerdict::Continue);
    }

    #[test]
    fn streak_past_the_budget_is_exhausted() {
        let mut policy = BlockPolicy::new(1);
        // First blocked round is tolerated (one nudge); the second exhausts it.
        assert_eq!(policy.record_round(&["read"]), ToolBlockVerdict::Continue);
        assert_eq!(
            policy.record_round(&["read"]),
            ToolBlockVerdict::Exhausted {
                name: "read".to_string()
            }
        );
    }

    #[test]
    fn zero_retries_exhausts_on_the_first_block() {
        let mut policy = BlockPolicy::new(0);
        assert_eq!(
            policy.record_round(&["read"]),
            ToolBlockVerdict::Exhausted {
                name: "read".to_string()
            }
        );
    }
}
