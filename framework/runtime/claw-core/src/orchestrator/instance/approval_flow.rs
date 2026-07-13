use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentCommand, AgentCommandError, AgentId, ApprovalDecision};

use super::scheduler::PendingApproval;
use super::{DriveOutput, OrchestratorInstance};

#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum ApprovalResolutionError {
    #[error("no active approval to resolve")]
    NoActiveApproval,
    #[error("no agent {0} to resolve approval for")]
    UnknownAgent(AgentId),
    #[error(transparent)]
    Command(AgentCommandError),
}

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(crate) fn active_approval(&self) -> Option<PendingApproval> {
        self.state.get().scheduler.active_approval()
    }

    pub(crate) fn resolve_active_approval(
        &mut self,
        decision: ApprovalDecision,
    ) -> Result<(), ApprovalResolutionError> {
        let pending = self
            .state
            .get()
            .scheduler
            .active_approval()
            .ok_or(ApprovalResolutionError::NoActiveApproval)?;
        let decision_name: &'static str = (&decision).into();
        self.registry
            .get_mut(pending.agent)
            .ok_or(ApprovalResolutionError::UnknownAgent(pending.agent))?
            .send_command(AgentCommand::ApprovalResult(decision))
            .map_err(ApprovalResolutionError::Command)?;

        let removed = self
            .state
            .get_mut()
            .scheduler
            .remove_approval(pending.agent);
        debug_assert!(
            removed,
            "the active approval changed without synchronization"
        );
        tracing::info!(
            name: "approval_resolved",
            agent = %pending.agent,
            decision = decision_name,
        );
        self.enqueue(pending.agent);
        Ok(())
    }

    pub(crate) fn take_next_approval_prompt(&mut self) -> DriveOutput {
        let Some(summary) = self.state.get_mut().scheduler.take_next_approval_summary() else {
            return DriveOutput::default();
        };
        DriveOutput::message(format!(
            "Permission approval needed:\n{summary}\n\nReply with approval or rejection."
        ))
    }

    pub(super) fn park_approval(&mut self, agent: AgentId, summary: String) -> DriveOutput {
        if !self.state.get().graph.contains(agent) {
            return DriveOutput::default();
        }
        self.state.get_mut().scheduler.park_approval(agent, summary);

        DriveOutput::default()
    }

    pub(super) fn has_unprompted_approval(&self) -> bool {
        self.state.get().scheduler.has_unprompted_approval()
    }
}

#[cfg(test)]
mod tests {
    use std::rc::Rc;
    use std::sync::{Arc, RwLock};

    use claw_interface::{ImmediateTimer, MemFs, RealHttp};
    use claw_tool::ToolRegistry;

    use crate::agent::{
        AgentCommandError, AgentId, AgentIdAllocator, AgentKind, AgentPlacement, ApprovalDecision,
        FsAgentFactory,
    };
    use crate::config::ClawApiManager;
    use crate::session::{SessionId, SessionPersistence};

    use super::super::{OrchestratorInstance, OrchestratorInstanceState, ROOT_AGENT_KIND};
    use super::ApprovalResolutionError;

    type TestInstance = OrchestratorInstance<MemFs, RealHttp, ImmediateTimer>;

    fn instance() -> TestInstance {
        MemFs::new();
        let factory = FsAgentFactory::new(
            Arc::new(ToolRegistry::new()),
            "/approval-test".to_owned(),
            Vec::new(),
            Arc::new(RwLock::new(ClawApiManager::new())),
        )
        .expect("test factory builds");
        OrchestratorInstance::new(
            SessionId::new(1),
            Rc::new(factory),
            AgentIdAllocator::new(),
            OrchestratorInstanceState::default(),
        )
    }

    #[test]
    fn unknown_agent_does_not_consume_active_approval() {
        let agent = AgentId(7);
        let mut instance = instance();
        instance
            .state
            .get_mut()
            .scheduler
            .park_approval(agent, "permission".to_owned());

        assert!(matches!(
            instance.resolve_active_approval(ApprovalDecision::Approved),
            Err(ApprovalResolutionError::UnknownAgent(id)) if id == agent
        ));
        assert_eq!(
            instance.active_approval().map(|pending| pending.agent),
            Some(agent)
        );
    }

    #[test]
    fn rejected_agent_command_does_not_consume_active_approval() {
        let agent = AgentId(7);
        let mut instance = instance();
        let kind = AgentKind::from_static(ROOT_AGENT_KIND);
        instance
            .build_agent(
                agent,
                &kind,
                String::new(),
                AgentPlacement::Root {
                    session: SessionId::new(1),
                    persistence: SessionPersistence::Ephemeral,
                },
                Vec::new(),
            )
            .expect("idle test agent builds");
        assert!(instance.state.get_mut().graph.insert_root(agent, kind));
        instance
            .state
            .get_mut()
            .scheduler
            .park_approval(agent, "permission".to_owned());

        assert!(matches!(
            instance.resolve_active_approval(ApprovalDecision::Approved),
            Err(ApprovalResolutionError::Command(
                AgentCommandError::NotAwaitingApproval { .. }
            ))
        ));
        assert_eq!(
            instance.active_approval().map(|pending| pending.agent),
            Some(agent)
        );
    }
}
