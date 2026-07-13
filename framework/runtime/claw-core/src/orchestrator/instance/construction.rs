use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use claw_checkpoint::{DurableState, PartStateSlice};
use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    AgentId, AgentIdAllocator, AgentKind, AgentPlacement, FsAgentCreateError, FsAgentFactory,
    GraphHost,
};
use crate::session::SessionId;

use super::inflight::InflightAgentTasks;
use super::model::{EffectQueue, InstanceHost, OrchestratorInstanceState, SnapshotView};
use super::persistence::OrchestratorInstanceRestoreError;
use super::OrchestratorInstance;

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Create an empty instance for `session`.
    pub(crate) fn new(
        session: SessionId,
        factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        state: OrchestratorInstanceState,
    ) -> Self {
        let effects: EffectQueue = Arc::new(Mutex::new(VecDeque::new()));
        let snapshots: SnapshotView = Arc::new(Mutex::new(HashMap::new()));
        let host: Arc<dyn GraphHost> = Arc::new(InstanceHost {
            agent_id_allocator: agent_id_allocator.clone(),
            effects: Arc::clone(&effects),
            snapshots: Arc::clone(&snapshots),
        });
        Self {
            session,
            factory,
            agent_id_allocator,
            state: DurableState::new(state),
            inflight: InflightAgentTasks::default(),
            effects,
            snapshots,
            host,
        }
    }

    pub(crate) fn from_restored_state(
        session: SessionId,
        factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        state: OrchestratorInstanceState,
    ) -> Result<Self, OrchestratorInstanceRestoreError> {
        let mut instance = Self::new(session, factory, agent_id_allocator, state);
        instance.restore_agents_from_pending_parts()?;
        Ok(instance)
    }

    fn restore_agents_from_pending_parts(
        &mut self,
    ) -> Result<(), OrchestratorInstanceRestoreError> {
        let pending = std::mem::take(&mut self.state.get_mut().pending_agent_parts);
        let agents = self
            .state
            .get()
            .meta
            .iter()
            .map(|(&id, meta)| (id, meta.clone()))
            .collect::<Vec<_>>();
        for (id, meta) in agents {
            let placement = if self.state.get().root == Some(id) {
                AgentPlacement::Root(self.session)
            } else {
                AgentPlacement::Sub(id)
            };
            self.build_agent(id, &meta.kind, String::new(), placement, Arc::from([]))
                .map_err(|source| OrchestratorInstanceRestoreError::agent(id, source))?;

            let Some(parts) = pending.get(&id) else {
                continue;
            };
            let state = self.state.get_mut();
            let agent = state
                .registry
                .get_mut(id)
                .ok_or_else(|| OrchestratorInstanceRestoreError::missing_agent(id))?;
            for part in parts {
                let restored = agent
                    .restore_durable_part(
                        &part.name,
                        PartStateSlice {
                            schema_version: part.schema_version,
                            bytes: &part.bytes,
                        },
                    )
                    .map_err(|source| {
                        OrchestratorInstanceRestoreError::durable_part(
                            id,
                            part.name.clone(),
                            source,
                        )
                    })?;
                if !restored {
                    return Err(OrchestratorInstanceRestoreError::unknown_part(
                        id,
                        part.name.clone(),
                    ));
                }
            }
        }
        Ok(())
    }

    pub(super) fn build_agent(
        &mut self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        placement: AgentPlacement,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<(), FsAgentCreateError> {
        let agent = self.factory.create_agent(
            id,
            kind,
            goal,
            placement,
            Arc::clone(&self.host),
            inherited_context,
        )?;
        self.state.get_mut().registry.insert(id, agent);
        Ok(())
    }
}
