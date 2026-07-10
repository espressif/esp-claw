use std::collections::VecDeque;

use claw_permission::GrantStore;
use serde::{Deserialize, Serialize};

use crate::agent::tools::ControlSignal;

use super::{
    AgentCommand, AgentCommandError, AgentState, ApprovalId, ApprovalIdAllocator,
    IterationIdAllocator,
};

/// Internal lifecycle state. Unlike the public [`AgentState`], this carries the
/// pending approval id in the state variant so the agent cannot be
/// `AwaitingApproval` without knowing which request it is waiting for.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
pub(super) enum AgentLifecycle {
    Idle,
    Running,
    AwaitingApproval(ApprovalId),
}

impl AgentLifecycle {
    fn public(self) -> AgentState {
        match self {
            Self::Idle => AgentState::Idle,
            Self::Running => AgentState::Running,
            Self::AwaitingApproval(_) => AgentState::AwaitingApproval,
        }
    }
}

/// One item on the agent's inbox: either an external [`AgentCommand`] or an
/// internal [`ControlSignal`] raised by a built-in tool. Both flow through the
/// one reducer, but only `Command` is constructible by outside callers.
#[derive(Clone, Deserialize, Serialize)]
pub(super) enum Inbound {
    Command(AgentCommand),
    TaskInput(String),
    Control(ControlSignal),
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(super) struct BlockPolicy {
    retries: u32,
    blocked_rounds: u32,
}

impl BlockPolicy {
    fn new(retries: u32) -> Self {
        Self {
            retries,
            blocked_rounds: 0,
        }
    }

    pub(super) fn record_round(&mut self, blocked: &[&str]) -> ToolBlockVerdict {
        if blocked.is_empty() {
            self.blocked_rounds = 0;
            return ToolBlockVerdict::Continue;
        }
        self.blocked_rounds = self.blocked_rounds.saturating_add(1);
        if self.blocked_rounds > self.retries {
            return ToolBlockVerdict::Exhausted {
                name: blocked[0].to_string(),
            };
        }
        ToolBlockVerdict::Continue
    }
}

impl Default for BlockPolicy {
    fn default() -> Self {
        Self::new(0)
    }
}

pub(super) enum ToolBlockVerdict {
    Continue,
    Exhausted { name: String },
}

#[derive(Deserialize, Serialize)]
pub(super) struct BaseAgentState {
    pub(super) block_policy: BlockPolicy,
    pub(super) permission_grants: GrantStore,
    pub(super) pending_grant_signatures: Vec<String>,
    pub(super) iterations: IterationIdAllocator,
    pub(super) approvals: ApprovalIdAllocator,
    pub(super) lifecycle: AgentLifecycle,
    pub(super) projected_lifecycle: AgentLifecycle,
    pub(super) inbox: VecDeque<Inbound>,
}

impl BaseAgentState {
    pub(super) fn new(block_retries: u32) -> Self {
        Self {
            block_policy: BlockPolicy::new(block_retries),
            permission_grants: GrantStore::new(),
            pending_grant_signatures: Vec::new(),
            iterations: IterationIdAllocator::new(),
            approvals: ApprovalIdAllocator::new(),
            lifecycle: AgentLifecycle::Idle,
            projected_lifecycle: AgentLifecycle::Idle,
            inbox: VecDeque::new(),
        }
    }
}

impl Default for BaseAgentState {
    fn default() -> Self {
        Self::new(0)
    }
}

/// The FSM transition table: the single authority on whether `command` is legal
/// in `state`, and what state it leads to.
///
/// The match is exhaustive over every `(state, command)` pair so a new state or
/// command cannot be silently mishandled.
pub(super) fn classify(
    state: AgentLifecycle,
    command: &AgentCommand,
) -> Result<AgentLifecycle, AgentCommandError> {
    use AgentCommand as Command;
    use AgentLifecycle as State;
    match (state, command) {
        // External append starts a fresh task only at an idle boundary. Active
        // task input is queued by the orchestrator until that boundary.
        (State::Idle, Command::AppendMessage(_)) => Ok(State::Running),
        (state @ (State::Running | State::AwaitingApproval(_)), Command::AppendMessage(_)) => {
            Err(AgentCommandError::CannotAppend {
                state: state.public(),
            })
        }
        // Cancel ends an active task; there is nothing to cancel when idle.
        (State::Idle, Command::Cancel { .. }) => Err(AgentCommandError::NothingToCancel),
        (State::Running | State::AwaitingApproval(_), Command::Cancel { .. }) => Ok(State::Idle),

        // An approval result needs a matching pending request.
        (State::AwaitingApproval(pending), Command::ApprovalResult { id, .. }) => {
            if pending == *id {
                Ok(State::Running)
            } else {
                Err(AgentCommandError::ApprovalMismatch {
                    expected: pending,
                    got: *id,
                })
            }
        }
        (state @ (State::Idle | State::Running), Command::ApprovalResult { .. }) => {
            Err(AgentCommandError::NotAwaitingApproval {
                state: state.public(),
            })
        }
    }
}
