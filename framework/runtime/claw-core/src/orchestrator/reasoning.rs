//! Per-session reasoning effort.
//!
//! Reasoning effort is an orchestration concern: the orchestrator owns it per
//! session and, once wired, shapes the prompt it builds and the api call it
//! makes. It lives here rather than in `claw-api` because it is the combined
//! result of orchestration and prompting, not a raw LLM-client parameter.
//!
//! Scaffold: the enum, the per-session state, and the "apply on the next turn"
//! seam exist. Translating an effort into prompt/api changes is not wired yet.

use serde::{Deserialize, Serialize};

/// How much reasoning effort a session asks the model to spend.
///
/// Higher tiers trade latency and token budget for deeper reasoning.
/// Reconfiguring a session mid-task takes effect on its next turn, not the one
/// already running (promoted at the `SessionDrive` turn boundary).
#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ReasoningEffort {
    /// Minimal reasoning; fastest and cheapest.
    Low,
    /// Balanced reasoning. The default.
    #[default]
    Medium,
    /// Extended reasoning for harder tasks.
    High,
    /// Maximum reasoning budget.
    Ultra,
}
