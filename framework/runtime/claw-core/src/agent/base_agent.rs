//! One configured agent task runner.
//!
//! Commands enter through [`BaseAgent::send_command`]. Each
//! [`BaseAgent::tick`] advances at most one iteration and returns a
//! [`TickOutcome`]; terminal outcomes leave the agent idle and reusable.

mod command;
mod construction;
mod control;
mod iteration;
mod persistence;
mod reducer;
mod state;
mod task_state;

use claw_api::{ClawApiAsync, RetryPolicy};
use claw_checkpoint::DurableState;
use claw_interface::{ClawHttp, ClawTimer};

use super::iteration_loop::IterationLoopError;
use crate::agent::tools::ControlSink;
use crate::memory::{ContextAdapter, Transcript};
use crate::protocol::IterationIdAllocator;
use claw_context::Context;
use claw_tool::ToolSet;

pub(crate) use self::command::{
    AgentCommand, AgentCommandError, AgentState, ApprovalDecision, BaseAgentBuildError, TickOutcome,
};
pub(super) use self::construction::BaseAgentConfig;
pub(crate) use self::control::AgentAbortHandle;
use self::control::AgentInterruption;
use self::state::BaseAgentState;

/// A base agent that runs one task at a time as a sequence of iterations.
pub(crate) struct BaseAgent<H: ClawHttp, Timer: ClawTimer> {
    llm: ClawApiAsync<H, Timer>,
    retry_policy: RetryPolicy,
    interruption: AgentInterruption,
    /// Type-erased so context adapters need not carry the filesystem type.
    transcript: Box<dyn Transcript>,
    tools: ToolSet,
    context: Context,
    state: DurableState<BaseAgentState>,
    outcome: Option<TickOutcome>,
    control: ControlSink,
    adapters: Vec<Box<dyn ContextAdapter>>,
}
