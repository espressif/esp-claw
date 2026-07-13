use claw_permission::GrantStore;
use serde::{Deserialize, Serialize};

use super::task_state::TaskState;
use super::IterationIdAllocator;

#[derive(Deserialize, Serialize)]
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

pub(super) enum ToolBlockVerdict {
    Continue,
    Exhausted { name: String },
}

#[derive(Deserialize, Serialize)]
pub(super) struct BaseAgentState {
    pub(super) block_policy: BlockPolicy,
    pub(super) permission_grants: GrantStore,
    pub(super) iterations: IterationIdAllocator,
    task: TaskState,
}

impl BaseAgentState {
    pub(super) fn new(block_retries: u32) -> Self {
        Self {
            block_policy: BlockPolicy::new(block_retries),
            permission_grants: GrantStore::new(),
            iterations: IterationIdAllocator::new(),
            task: TaskState::new(),
        }
    }

    pub(super) fn task(&self) -> &TaskState {
        &self.task
    }

    pub(super) fn task_mut(&mut self) -> &mut TaskState {
        &mut self.task
    }
}
