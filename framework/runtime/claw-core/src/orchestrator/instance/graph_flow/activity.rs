use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    AgentCommand, AgentId, AgentKind, AgentPlacement, AgentSnapshot, AgentStatus, CancelReason,
    TerminationPolicy,
};
use crate::orchestrator::InstanceWork;

use super::super::model::{
    AgentMessageDeliveryError, InstanceDeliverError, NodeMeta, ROOT_AGENT_KIND,
};
use super::super::OrchestratorInstance;

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Deliver a user message to this session's root.
    pub(crate) fn deliver(&mut self, text: impl Into<String>) -> Result<(), InstanceDeliverError> {
        match self.state.get().root {
            Some(root) => self
                .deliver_message(root, text)
                .map_err(|source| InstanceDeliverError::Root { root, source }),
            None => {
                let id = self.agent_id_allocator.next();
                let kind = AgentKind::new(ROOT_AGENT_KIND);
                self.build_agent(id, &kind, text.into(), AgentPlacement::Root(self.session))?;
                self.state.get_mut().meta.insert(
                    id,
                    NodeMeta {
                        parent: None,
                        depth: 0,
                        kind,
                        name: None,
                        termination: TerminationPolicy::AutoOnIdle,
                    },
                );
                self.state.get_mut().root = Some(id);
                self.enqueue(id);
                Ok(())
            }
        }
    }

    pub(crate) fn cancel_all(&mut self, reason: CancelReason) {
        let agents: Vec<AgentId> = self.state.get().meta.keys().copied().collect();
        for agent_id in agents {
            let Some(agent) = self.state.get_mut().registry.get_mut(agent_id) else {
                continue;
            };
            if agent
                .send_command(AgentCommand::Cancel {
                    reason: reason.clone(),
                })
                .is_ok()
            {
                self.enqueue(agent_id);
            }
        }
    }

    pub(crate) fn work(&self) -> InstanceWork {
        if self.has_root_work() || self.has_unprompted_approval() {
            InstanceWork::Root
        } else if self.has_background_work() {
            InstanceWork::Background
        } else {
            InstanceWork::None
        }
    }

    pub(in crate::orchestrator::instance) fn enqueue(&mut self, id: AgentId) {
        if !self.state.get().ready.contains(&id) {
            self.state.get_mut().ready.push_back(id);
        }
    }

    pub(in crate::orchestrator::instance) fn has_ready(&self) -> bool {
        !self.state.get().ready.is_empty()
    }

    pub(in crate::orchestrator::instance) fn has_root_work(&self) -> bool {
        let Some(root) = self.state.get().root else {
            return false;
        };
        self.state.get().ready.contains(&root) || self.inflight.has_root()
    }

    pub(in crate::orchestrator::instance) fn refresh_snapshots(&self) {
        let mut snapshots = self
            .snapshots
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        snapshots.clear();
        for (&id, meta) in &self.state.get().meta {
            let status = if self.state.get().parked_approvals.contains_key(&id) {
                AgentStatus::AwaitingApproval
            } else if self.state.get().ready.contains(&id) {
                AgentStatus::Ready
            } else {
                AgentStatus::Idle
            };
            snapshots.insert(
                id,
                AgentSnapshot {
                    id,
                    kind: meta.kind.clone(),
                    name: meta.name.clone(),
                    parent: meta.parent,
                    depth: meta.depth,
                    termination: meta.termination,
                    status,
                },
            );
        }
    }

    fn deliver_message(
        &mut self,
        id: AgentId,
        text: impl Into<String>,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.state.get_mut().registry.get_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        agent.send_command(AgentCommand::AppendMessage(text.into()))?;
        self.enqueue(id);
        Ok(())
    }

    fn has_background_work(&self) -> bool {
        let root = self.state.get().root;
        self.state.get().ready.iter().any(|id| Some(*id) != root) || self.inflight.has_background()
    }
}
