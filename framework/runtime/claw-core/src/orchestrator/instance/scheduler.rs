use std::collections::VecDeque;

use crate::agent::{AgentId, AgentStatus};

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PendingApproval {
    pub(crate) agent: AgentId,
    pub(crate) summary: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct ParkedApproval {
    pub(super) summary: String,
    pub(super) prompted: bool,
}

pub(super) struct SubagentResult {
    pub(super) parent: AgentId,
    pub(super) child: AgentId,
    pub(super) text: String,
    pub(super) ok: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum InstanceWork {
    None,
    Root,
    Background,
}

#[derive(Default)]
pub(super) struct SchedulerState {
    ready: VecDeque<AgentId>,
    approvals: VecDeque<(AgentId, ParkedApproval)>,
    subagent_result_mailbox: VecDeque<SubagentResult>,
}

impl SchedulerState {
    pub(super) fn restored(
        ready: VecDeque<AgentId>,
        approvals: VecDeque<(AgentId, ParkedApproval)>,
        subagent_result_mailbox: VecDeque<SubagentResult>,
    ) -> Self {
        Self {
            ready,
            approvals,
            subagent_result_mailbox,
        }
    }

    pub(super) fn enqueue(&mut self, id: AgentId) {
        if !self.ready.contains(&id) {
            self.ready.push_back(id);
        }
    }

    pub(super) fn has_ready(&self) -> bool {
        !self.ready.is_empty()
    }

    pub(super) fn is_ready(&self, id: AgentId) -> bool {
        self.ready.contains(&id)
    }

    pub(super) fn pop_ready(&mut self) -> Option<AgentId> {
        self.ready.pop_front()
    }

    pub(super) fn clear_turn_work(&mut self) {
        self.ready.clear();
        self.approvals.clear();
        self.subagent_result_mailbox.clear();
    }

    pub(super) fn agent_status(&self, id: AgentId, running: bool) -> AgentStatus {
        if self.is_awaiting_approval(id) {
            AgentStatus::AwaitingApproval
        } else if running {
            AgentStatus::Running
        } else if self.ready.contains(&id) {
            AgentStatus::Ready
        } else {
            AgentStatus::Idle
        }
    }

    pub(super) fn work(
        &self,
        root: Option<AgentId>,
        root_running: bool,
        background_running: bool,
    ) -> InstanceWork {
        let root_ready = root.is_some_and(|root| self.ready.contains(&root));
        if root_ready || root_running || self.has_unprompted_approval() {
            return InstanceWork::Root;
        }
        let background_ready = self.ready.iter().any(|id| Some(*id) != root);
        if background_ready || background_running {
            InstanceWork::Background
        } else {
            InstanceWork::None
        }
    }

    pub(super) fn has_unprompted_approval(&self) -> bool {
        self.approvals
            .front()
            .is_some_and(|(_, pending)| !pending.prompted)
    }

    pub(super) fn active_approval(&self) -> Option<PendingApproval> {
        let (agent, pending) = self.approvals.front()?;
        Some(PendingApproval {
            agent: *agent,
            summary: pending.summary.clone(),
        })
    }

    pub(super) fn take_next_approval_summary(&mut self) -> Option<String> {
        let (_, pending) = self.approvals.front_mut()?;
        if pending.prompted {
            return None;
        }
        pending.prompted = true;
        Some(pending.summary.clone())
    }

    pub(super) fn park_approval(&mut self, agent: AgentId, summary: String) {
        let replacement = ParkedApproval {
            summary,
            prompted: false,
        };
        if let Some((_, pending)) = self
            .approvals
            .iter_mut()
            .find(|(queued_agent, _)| *queued_agent == agent)
        {
            *pending = replacement;
        } else {
            self.approvals.push_back((agent, replacement));
        }
    }

    pub(super) fn is_awaiting_approval(&self, agent: AgentId) -> bool {
        self.approvals
            .iter()
            .any(|(queued_agent, _)| *queued_agent == agent)
    }

    pub(super) fn remove_approval(&mut self, agent: AgentId) -> bool {
        let Some(position) = self
            .approvals
            .iter()
            .position(|(queued_agent, _)| *queued_agent == agent)
        else {
            return false;
        };
        self.approvals.remove(position).is_some()
    }

    pub(super) fn remove_agents(&mut self, agents: &[AgentId]) {
        self.approvals.retain(|(agent, _)| !agents.contains(agent));
        self.ready.retain(|queued| !agents.contains(queued));
        self.subagent_result_mailbox
            .retain(|result| !agents.contains(&result.parent) && !agents.contains(&result.child));
    }

    pub(super) fn queue_subagent_result(&mut self, result: SubagentResult) {
        self.subagent_result_mailbox.push_back(result);
    }

    pub(super) fn has_subagent_results(&self) -> bool {
        !self.subagent_result_mailbox.is_empty()
    }

    pub(super) fn take_subagent_results(&mut self) -> VecDeque<SubagentResult> {
        std::mem::take(&mut self.subagent_result_mailbox)
    }

    pub(super) fn replace_subagent_results(&mut self, results: VecDeque<SubagentResult>) {
        self.subagent_result_mailbox = results;
    }

    pub(super) fn ready_ids(&self) -> impl Iterator<Item = AgentId> + '_ {
        self.ready.iter().copied()
    }

    pub(super) fn approvals(&self) -> impl Iterator<Item = (AgentId, &ParkedApproval)> {
        self.approvals
            .iter()
            .map(|(agent, approval)| (*agent, approval))
    }

    pub(super) fn subagent_results(&self) -> impl Iterator<Item = &SubagentResult> {
        self.subagent_result_mailbox.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::{InstanceWork, SchedulerState};
    use crate::agent::{AgentId, AgentStatus};

    #[test]
    fn scheduler_centrally_computes_agent_status() {
        let ready = AgentId(1);
        let running = AgentId(2);
        let awaiting = AgentId(3);
        let idle = AgentId(4);
        let mut scheduler = SchedulerState::default();
        scheduler.enqueue(ready);
        scheduler.park_approval(awaiting, "permission".to_owned());

        assert_eq!(scheduler.agent_status(ready, false), AgentStatus::Ready);
        assert_eq!(scheduler.agent_status(running, true), AgentStatus::Running);
        assert_eq!(
            scheduler.agent_status(awaiting, false),
            AgentStatus::AwaitingApproval
        );
        assert_eq!(scheduler.agent_status(idle, false), AgentStatus::Idle);
    }

    #[test]
    fn scheduler_owns_work_classification() {
        let root = AgentId(1);
        let child = AgentId(2);
        let mut scheduler = SchedulerState::default();

        assert_eq!(scheduler.work(Some(root), false, false), InstanceWork::None);
        scheduler.enqueue(child);
        assert_eq!(
            scheduler.work(Some(root), false, false),
            InstanceWork::Background
        );
        scheduler.enqueue(root);
        assert_eq!(scheduler.work(Some(root), false, false), InstanceWork::Root);
    }

    #[test]
    fn approvals_are_prompted_and_removed_in_queue_order() {
        let first = AgentId(1);
        let second = AgentId(2);
        let mut scheduler = SchedulerState::default();
        scheduler.park_approval(first, "first".to_owned());
        scheduler.park_approval(second, "second".to_owned());

        assert_eq!(
            scheduler.take_next_approval_summary().as_deref(),
            Some("first")
        );
        assert!(!scheduler.has_unprompted_approval());
        assert!(scheduler.remove_approval(first));
        assert!(scheduler.has_unprompted_approval());
        assert_eq!(
            scheduler.take_next_approval_summary().as_deref(),
            Some("second")
        );
    }
}
