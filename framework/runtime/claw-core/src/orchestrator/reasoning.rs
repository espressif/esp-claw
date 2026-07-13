//! Per-session reasoning effort.
//!
//! Reasoning effort is a prompting-only orchestration concern. The orchestrator
//! owns it per session and projects it into the root agent's context at each turn
//! boundary; it never changes the provider API request shape.

use claw_context::{Block, BlockKind};
use serde::{Deserialize, Serialize};

const LOW_PROMPT: &str = include_str!("../../resources/prompt/effort/low.md");
const MEDIUM_PROMPT: &str = include_str!("../../resources/prompt/effort/medium.md");
const HIGH_PROMPT: &str = include_str!("../../resources/prompt/effort/high.md");
const ULTRA_PROMPT: &str = include_str!("../../resources/prompt/effort/ultra.md");

/// How deliberately a session asks its root agent to orchestrate work.
///
/// Higher tiers prompt more decomposition, delegation, and verification.
/// Reconfiguring a session mid-task takes effect on its next turn, not the one
/// already running (promoted at the `SessionDrive` turn boundary).
#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ReasoningEffort {
    /// Take the shortest sound path and avoid delegation by default.
    Low,
    /// Use necessary steps and delegate only clearly separable work. The default.
    #[default]
    Medium,
    /// Deliberately decompose, delegate, and verify non-trivial work.
    High,
    /// Use multi-agent execution and independent verification when appropriate.
    Ultra,
}

impl ReasoningEffort {
    pub(crate) fn context_block(self) -> Block<'static> {
        let content = match self {
            Self::Low => LOW_PROMPT,
            Self::Medium => MEDIUM_PROMPT,
            Self::High => HIGH_PROMPT,
            Self::Ultra => ULTRA_PROMPT,
        };
        Block::new(BlockKind::ReasoningEffort, content)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_effort_has_a_reasoning_effort_context_block() {
        for effort in [
            ReasoningEffort::Low,
            ReasoningEffort::Medium,
            ReasoningEffort::High,
            ReasoningEffort::Ultra,
        ] {
            let block = effort.context_block();
            assert_eq!(block.kind, BlockKind::ReasoningEffort);
            assert!(!block.content.trim().is_empty());
        }
    }
}
