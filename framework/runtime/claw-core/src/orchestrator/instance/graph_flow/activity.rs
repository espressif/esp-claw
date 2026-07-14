use crate::agent::{
    AgentCommand, AgentCommandError, AgentId, AgentKind, AgentPlacement, FsAgentCreateError,
};
use crate::session::{Message, SessionPersistence};
use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use super::super::{OrchestratorInstance, ROOT_AGENT_KIND};

#[derive(Debug, thiserror::Error)]
pub(crate) enum InstanceDeliverError {
    #[error("failed to build root agent: {0}")]
    Create(#[from] FsAgentCreateError),
    #[error("failed to deliver to root {root}: {source}")]
    Root {
        root: AgentId,
        #[source]
        source: AgentMessageDeliveryError,
    },
}

#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentMessageDeliveryError {
    #[error("no such agent: {0}")]
    UnknownAgent(AgentId),
    #[error(transparent)]
    Command(#[from] AgentCommandError),
}

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Deliver a message to this session's root.
    pub(crate) fn deliver(
        &mut self,
        message: Message,
        reasoning_effort: Block<'static>,
        persistence: SessionPersistence,
    ) -> Result<(), InstanceDeliverError> {
        match self.state.get().graph.root() {
            Some(root) => {
                self.set_agent_context_block(root, reasoning_effort)
                    .map_err(|source| InstanceDeliverError::Root { root, source })?;
                self.deliver_message(root, message)
                    .map_err(|source| InstanceDeliverError::Root { root, source })
            }
            None => {
                let id = self.agent_id_allocator.next();
                let kind = AgentKind::from_static(ROOT_AGENT_KIND);
                self.build_agent(
                    id,
                    &kind,
                    message,
                    AgentPlacement::Root {
                        session: self.session,
                        persistence,
                    },
                    vec![reasoning_effort],
                )?;
                let inserted = self.state.get_mut().graph.insert_root(id, kind);
                debug_assert!(inserted, "root insertion requires an empty graph");
                self.enqueue(id);
                Ok(())
            }
        }
    }

    pub(crate) fn set_root_context_block(
        &mut self,
        block: Block<'static>,
    ) -> Result<(), InstanceDeliverError> {
        let Some(root) = self.state.get().graph.root() else {
            return Ok(());
        };
        self.set_agent_context_block(root, block)
            .map_err(|source| InstanceDeliverError::Root { root, source })
    }

    pub(crate) fn cancel_all(&mut self) {
        let agents: Vec<AgentId> = self.state.get().graph.agent_ids().collect();
        for agent_id in agents {
            let Some(agent) = self.registry.get_mut(agent_id) else {
                continue;
            };
            if agent.send_command(AgentCommand::Cancel).is_ok() {
                self.enqueue(agent_id);
            }
        }
    }

    fn deliver_message(
        &mut self,
        id: AgentId,
        message: Message,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.registry.get_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        agent.send_command(AgentCommand::AppendMessage(message))?;
        self.enqueue(id);
        Ok(())
    }

    fn set_agent_context_block(
        &mut self,
        id: AgentId,
        block: Block<'static>,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.registry.get_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        agent.set_context_block(block);
        Ok(())
    }

    /// Cancel the target's active task (if any) and start a fresh one.
    ///
    /// `Cancel` and `AppendMessage` are batched on the agent inbox so a lone
    /// cancel never surfaces as [`TickOutcome::Cancelled`] to the lifecycle
    /// router (which would delete a subagent subtree).
    pub(in crate::orchestrator::instance) fn deliver_followup(
        &mut self,
        id: AgentId,
        message: Message,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.registry.get_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        let _ = agent.send_command(AgentCommand::Cancel);
        agent.send_command(AgentCommand::AppendMessage(message))?;
        self.enqueue(id);
        tracing::info!(name: "followup_delivered", target_agent = %id);
        Ok(())
    }
}
