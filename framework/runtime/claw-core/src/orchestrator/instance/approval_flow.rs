use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentCommand, AgentId, ApprovalDecision, ApprovalId};

use super::model::{ApprovalResolutionError, DriveOutput, ParkedApproval, PendingApproval};
use super::OrchestratorInstance;

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(crate) fn active_approval(&self) -> Option<PendingApproval> {
        let agent = *self.state.get().approval_queue.front()?;
        let pending = self.state.get().parked_approvals.get(&agent)?;
        Some(PendingApproval {
            agent,
            approval: pending.approval,
            summary: pending.summary.clone(),
        })
    }

    pub(crate) fn resolve_active_approval(
        &mut self,
        decision: ApprovalDecision,
    ) -> Result<(), ApprovalResolutionError> {
        let pending = self
            .pop_active_approval()
            .ok_or(ApprovalResolutionError::NoActiveApproval)?;
        self.send_approval_decision(pending.agent, pending.approval, decision)
    }

    pub(crate) fn take_next_approval_prompt(&mut self) -> DriveOutput {
        loop {
            let Some(agent) = self.state.get().approval_queue.front().copied() else {
                return DriveOutput::default();
            };
            let Some(pending) = self.state.get_mut().parked_approvals.get_mut(&agent) else {
                self.state.get_mut().approval_queue.pop_front();
                continue;
            };
            if pending.prompted {
                return DriveOutput::default();
            }
            let summary = pending.summary.clone();
            pending.prompted = true;
            return DriveOutput::reply(
                self.session,
                format!(
                    "Permission approval needed:\n{summary}\n\nReply with approval or rejection."
                ),
            );
        }
    }

    pub(super) fn park_approval(
        &mut self,
        agent: AgentId,
        approval: ApprovalId,
        summary: String,
    ) -> DriveOutput {
        if !self.state.get().meta.contains_key(&agent) {
            return DriveOutput::default();
        }
        self.state.get_mut().parked_approvals.insert(
            agent,
            ParkedApproval {
                approval,
                summary,
                prompted: false,
            },
        );
        if !self.state.get().approval_queue.contains(&agent) {
            self.state.get_mut().approval_queue.push_back(agent);
        }

        DriveOutput::default()
    }

    pub(super) fn has_unprompted_approval(&self) -> bool {
        self.state.get().approval_queue.iter().any(|agent| {
            self.state
                .get()
                .parked_approvals
                .get(agent)
                .is_some_and(|pending| !pending.prompted)
        })
    }

    fn send_approval_decision(
        &mut self,
        agent: AgentId,
        approval: ApprovalId,
        decision: ApprovalDecision,
    ) -> Result<(), ApprovalResolutionError> {
        let decision_name: &'static str = (&decision).into();
        self.state
            .get_mut()
            .registry
            .get_mut(agent)
            .ok_or(ApprovalResolutionError::UnknownAgent(agent))?
            .send_command(AgentCommand::ApprovalResult {
                id: approval,
                decision,
            })
            .map_err(ApprovalResolutionError::Command)?;
        tracing::info!(
            name: "approval_resolved",
            approval = %approval,
            decision = decision_name,
        );
        self.enqueue(agent);
        Ok(())
    }

    fn pop_active_approval(&mut self) -> Option<PendingApproval> {
        while let Some(agent) = self.pop_approval_queue() {
            let Some(pending) = self.state.get_mut().parked_approvals.remove(&agent) else {
                continue;
            };
            return Some(PendingApproval {
                agent,
                approval: pending.approval,
                summary: pending.summary,
            });
        }
        None
    }

    fn pop_approval_queue(&mut self) -> Option<AgentId> {
        if self.state.get().approval_queue.is_empty() {
            return None;
        }
        self.state.get_mut().approval_queue.pop_front()
    }

    #[cfg(test)]
    pub(crate) fn install_test_approval(&mut self) {
        let agent = AgentId::new(1);
        self.state.get_mut().parked_approvals.insert(
            agent,
            super::model::ParkedApproval {
                approval: crate::agent::ApprovalId::new(1),
                summary: "test approval".to_string(),
                prompted: true,
            },
        );
        self.state.get_mut().approval_queue.push_back(agent);
    }
}
