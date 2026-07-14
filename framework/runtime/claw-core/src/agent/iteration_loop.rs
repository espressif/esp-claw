//! One LLM/tool round-trip per [`IterationLoop`].
//!
//! Layer 3 only: pass chat + a `ToolSet`, LLM picks tool calls, we invoke
//! them, return which tools ran. No session, channel, or routing concepts here.
//!
//! On preemption this layer only detects the signal and ends the iteration.
//! It does not read, format, or return interrupt message content — upper layers
//! own pending input and context rebuild.

mod run;
mod tool_round;
mod types;

use claw_api::{ClawApiAsync, RetryPolicy};
use claw_interface::{ClawHttp, ClawTimer};

use crate::protocol::EventSink;

pub(crate) use types::{
    AppendedMessages, CompletedKind, CompletedOutcome, InterruptionControl, IterationLoopError,
    IterationOutcome, IterationResult, IterationStep, PreemptedOutcome, ToolRun, ToolsOutcome,
};

/// One-step executor: LLM + preempt control only. Tools live on [`IterationStep`].
///
/// Generic over the HTTP transport `H` so the LLM call stays statically
/// dispatched. The loop borrows the agent's [`ClawApiAsync`] mutably for exactly one
/// `chat` round, so it is consumed by [`run`](Self::run).
pub(crate) struct IterationLoop<'a, H: ClawHttp, Timer: ClawTimer> {
    pub llm: &'a mut ClawApiAsync<H, Timer>,
    pub interruption: &'a dyn InterruptionControl,
    /// Retry policy applied to this iteration's LLM call (see [`RetryPolicy`]).
    pub retry: RetryPolicy,
    /// Where this iteration's `SessionEvent`s are pushed. The owner may disable
    /// it when this agent's internal stream should not be public.
    pub events: &'a EventSink,
}
