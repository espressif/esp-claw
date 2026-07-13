use serde::{Deserialize, Serialize};
use strum::IntoStaticStr;

use claw_tool::ToolSetError;

use super::IterationLoopError;

/// Inbound: a control input handed to the agent. This is the agent's entire
/// external surface — the outside drives the agent only through these.
///
/// Notably there is **no `Preempt` command**: the cooperative abort path is the
/// separate [`super::AgentAbortHandle`] and carries no message payload. New information
/// arrives as [`AppendMessage`](Self::AppendMessage) only at an idle boundary;
/// hard task termination is [`Cancel`](Self::Cancel).
#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
pub(crate) enum AgentCommand {
    /// Start a fresh task with a user message. This is valid only when the agent
    /// is idle; the orchestrator is responsible for deferring append delivery
    /// until that boundary.
    AppendMessage(String),
    /// Abandon the current task. (Orchestrator-initiated hard stop — distinct
    /// from the agent ending itself via `conversation_end`.) Being disruptive,
    /// it discards the still-open turn instead of writing a marker, so cancelled
    /// partial work leaves no transcript trace.
    Cancel,
    /// Deliver the human's decision for the active approval.
    ApprovalResult(ApprovalDecision),
}

/// The agent's externally observable lifecycle state.
///
/// Exposed so a driver can read which state a rejected command hit off an
/// [`AgentCommandError`]. `Idle` means "no active task, awaiting input" — both
/// before the first task and after one finishes (terminal outcomes leave the
/// agent idle and reusable).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AgentState {
    /// No active iteration; waiting for an [`AppendMessage`](AgentCommand::AppendMessage).
    Idle,
    /// A task is actively iterating.
    Running,
    /// Waiting on a permission-policy `Ask`, awaiting an
    /// [`ApprovalResult`](AgentCommand::ApprovalResult).
    AwaitingApproval,
}

/// Rejection of an [`AgentCommand`] that is invalid for the agent's current
/// [`AgentState`].
///
/// The agent is a state machine; not every command is meaningful in every
/// state (e.g. [`Cancel`](AgentCommand::Cancel) while the agent is already
/// idle). A rejected command is
/// **not** enqueued and the agent is left unchanged, so the caller can react
/// without racing a `tick`. Validation is against the state the agent *will* be
/// in once already-queued commands are applied, so batching commands between
/// ticks is sound.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub(crate) enum AgentCommandError {
    /// [`AppendMessage`](AgentCommand::AppendMessage) is only accepted at an idle
    /// boundary. Active-task input must be deferred by the driver.
    #[error("cannot append: the agent is {state:?}, not idle")]
    CannotAppend {
        /// The state the agent was in when append was rejected.
        state: AgentState,
    },
    /// [`Cancel`](AgentCommand::Cancel) has nothing to act on while
    /// [`Idle`](AgentState::Idle).
    #[error("cannot cancel: the agent is idle with no active task")]
    NothingToCancel,
    /// [`ApprovalResult`](AgentCommand::ApprovalResult) is only valid while
    /// [`AwaitingApproval`](AgentState::AwaitingApproval).
    #[error("cannot resolve approval: the agent is {state:?}, not awaiting approval")]
    NotAwaitingApproval {
        /// The state the agent was in when the approval result was rejected.
        state: AgentState,
    },
}

/// A human's answer to an approval request.
#[derive(Clone, Debug, Deserialize, IntoStaticStr, PartialEq, Eq, Serialize)]
pub(crate) enum ApprovalDecision {
    /// The human approved; the agent continues.
    #[strum(serialize = "approved")]
    Approved,
    /// The human rejected, with a reason recorded for the agent to reconsider.
    #[strum(serialize = "rejected")]
    Rejected(String),
}

/// What one [`tick`](BaseAgent::tick) did — the agent's sole output channel.
///
/// `Working`/`Idle` are liveness for the driver loop; the rest are one-shot
/// results reported on the tick that produced them. A single tick yields exactly
/// one of these (tool execution is internal — it shows up only as `Working`).
#[derive(Clone, Debug)]
#[must_use]
pub(crate) enum TickOutcome {
    /// Progress was made; call `tick` again promptly.
    Working,
    /// Nothing to do right now (waiting for input or awaiting approval).
    Idle,
    /// The model returned a user-facing answer and handed control back.
    /// **Non-terminal** — the agent goes idle awaiting the next message.
    Yielded {
        /// The model's user-facing answer.
        text: String,
    },
    /// A tool call's permission policy returned `Ask`; the agent is waiting for a
    /// human decision. Resolve it by sending [`AgentCommand::ApprovalResult`].
    AwaitingApproval {
        /// A human-readable description of what needs approving.
        summary: String,
    },
    /// Terminal: the agent ended the task itself (via `conversation_end`). The
    /// agent returns to idle and may be re-tasked.
    Ended {
        /// The agent's closing message.
        final_message: String,
    },
    /// Terminal: the task was cancelled by the orchestrator.
    Cancelled,
    /// Terminal: the task failed.
    Failed(AgentRunError),
}

/// Cause of a terminal [`TickOutcome::Failed`].
///
/// Wraps the lower-level errors a tick can hit: a failed LLM/tool iteration, or a
/// tool refused past the soft-hide retry budget. Context assembly is driven by
/// adapters; adapter-local failures are logged at the adapter boundary, and
/// [`claw_context::Context::request`] is infallible, so the tick never fails on
/// context assembly.
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentRunError {
    /// One LLM/tool iteration failed.
    #[error(transparent)]
    Iteration(#[from] IterationLoopError),
    /// The model kept calling a tool that soft-hide gating does not permit this
    /// phase, past the allowed retry budget.
    #[error("tool not permitted in the current phase: {name}")]
    ToolNotPermitted {
        /// The name of the refused tool.
        name: String,
    },
    /// The private task phase/mailbox invariant was violated.
    #[error("agent task state invariant violated")]
    TaskStateInvariant,
}

/// Failure assembling a [`super::BaseAgent`] in [`super::BaseAgent::build`].
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum BaseAgentBuildError {
    /// Merging the built-in tool group onto the caller's tools hit a name clash.
    #[error(transparent)]
    Tools(#[from] ToolSetError),
}
