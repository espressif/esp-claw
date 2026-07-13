use crate::agent::{
    AgentCommand, AgentGraphSnapshot, AgentId, AgentKind, AgentPlacement, AgentSnapshot,
};
use crate::session::SessionPersistence;
use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use super::super::error::{AgentMessageDeliveryError, InstanceDeliverError};
use super::super::scheduler::InstanceWork;
use super::super::{OrchestratorInstance, ROOT_AGENT_KIND};

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Deliver a user message to this session's root.
    pub(crate) fn deliver(
        &mut self,
        text: String,
        reasoning_effort: Block<'static>,
        persistence: SessionPersistence,
    ) -> Result<(), InstanceDeliverError> {
        match self.state.get().graph.root() {
            Some(root) => {
                self.set_agent_context_block(root, reasoning_effort)
                    .map_err(|source| InstanceDeliverError::Root { root, source })?;
                self.deliver_message(root, text)
                    .map_err(|source| InstanceDeliverError::Root { root, source })
            }
            None => {
                let id = self.agent_id_allocator.next();
                let kind = AgentKind::from_static(ROOT_AGENT_KIND);
                self.build_agent(
                    id,
                    &kind,
                    text,
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

    pub(in crate::orchestrator::instance) fn clear_turn_work(&mut self) {
        let state = self.state.get_mut();
        state.scheduler.clear_turn_work();
        self.effects
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .clear();
    }

    pub(crate) fn work(&self) -> InstanceWork {
        self.state.get().scheduler.work(
            self.state.get().graph.root(),
            self.inflight.has_root(),
            self.inflight.has_background(),
        )
    }

    pub(in crate::orchestrator::instance) fn enqueue(&mut self, id: AgentId) {
        self.state.get_mut().scheduler.enqueue(id);
    }

    pub(in crate::orchestrator::instance) fn has_ready(&self) -> bool {
        self.state.get().scheduler.has_ready()
    }

    pub(in crate::orchestrator::instance) fn has_root_work(&self) -> bool {
        let Some(root) = self.state.get().graph.root() else {
            return false;
        };
        self.state.get().scheduler.is_ready(root) || self.inflight.has_root()
    }

    pub(in crate::orchestrator::instance) fn refresh_snapshots(&self) {
        let snapshot = AgentGraphSnapshot::new(self.state.get().graph.nodes().map(|(id, meta)| {
            AgentSnapshot {
                id,
                kind: meta.kind().clone(),
                name: meta.name().map(str::to_owned),
                parent: meta.parent(),
                depth: self
                    .state
                    .get()
                    .graph
                    .depth(id)
                    .expect("live graph topology is valid"),
                termination: meta.termination(),
                status: self
                    .state
                    .get()
                    .scheduler
                    .agent_status(id, self.inflight.contains(id)),
            }
        }));
        *self
            .snapshots
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = snapshot;
    }

    fn deliver_message(
        &mut self,
        id: AgentId,
        text: String,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.registry.get_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        agent.send_command(AgentCommand::AppendMessage(text))?;
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
        message: String,
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
