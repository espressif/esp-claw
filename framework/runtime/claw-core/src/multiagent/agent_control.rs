use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentCommand, AgentCommandError, FsAgentCreateError};
use crate::protocol::{AgentId, AgentKind, Message, SessionPersistence};

use super::{AgentPlacement, MultiagentRuntime, ROOT_AGENT_KIND};

#[derive(Debug, thiserror::Error)]
pub(crate) enum MultiagentDeliverError {
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

impl<Filesystem, Http, Timer> MultiagentRuntime<Filesystem, Http, Timer>
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
    ) -> Result<(), MultiagentDeliverError> {
        match self.state.get().root() {
            Some(root) => {
                self.set_agent_context_block(root, reasoning_effort)
                    .map_err(|source| MultiagentDeliverError::Root { root, source })?;
                self.deliver_message(root, message)
                    .map_err(|source| MultiagentDeliverError::Root { root, source })
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
                let inserted = self.state.get_mut().insert_root(id, kind);
                debug_assert!(inserted, "root insertion requires an empty graph");
                self.enqueue(id);
                Ok(())
            }
        }
    }

    pub(crate) fn set_root_context_block(
        &mut self,
        block: Block<'static>,
    ) -> Result<(), MultiagentDeliverError> {
        let Some(root) = self.state.get().root() else {
            return Ok(());
        };
        self.set_agent_context_block(root, block)
            .map_err(|source| MultiagentDeliverError::Root { root, source })
    }

    pub(crate) fn cancel_all(&mut self) {
        let agents: Vec<AgentId> = self.state.get().agent_ids().collect();
        for agent_id in agents {
            let Some(agent) = self.slots.available_agent_mut(agent_id) else {
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
        let Some(agent) = self.slots.available_agent_mut(id) else {
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
        let Some(agent) = self.slots.available_agent_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        agent.set_context_block(block);
        Ok(())
    }
}
