use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::rc::Rc;
use std::sync::{Arc, Mutex};

use claw_checkpoint::{DurableState, PartStateSlice};
use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    AgentGraphSnapshot, AgentId, AgentIdAllocator, AgentKind, AgentPlacement, FsAgentCreateError,
    FsAgentFactory, GraphHost,
};
use crate::session::{SessionId, SessionPersistence};

use super::graph_state::{EffectQueue, InstanceHost, SnapshotView};
use super::persistence::{
    AgentPartState, OrchestratorInstanceRestore, OrchestratorInstanceRestoreError,
};
use super::{AgentRegistry, InflightAgentTasks, OrchestratorInstance, OrchestratorInstanceState};

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Create an empty instance for `session`.
    pub(crate) fn new(
        session: SessionId,
        factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        state: OrchestratorInstanceState,
    ) -> Self {
        let effects: EffectQueue = Arc::new(Mutex::new(VecDeque::new()));
        let snapshots: SnapshotView = Arc::new(Mutex::new(AgentGraphSnapshot::default()));
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
            registry: AgentRegistry::new(),
            inflight: InflightAgentTasks::new(),
            effects,
            snapshots,
            host,
        }
    }

    pub(crate) fn from_restored_state(
        session: SessionId,
        factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        restored: OrchestratorInstanceRestore,
    ) -> Result<Self, OrchestratorInstanceRestoreError> {
        let OrchestratorInstanceRestore { state, agent_parts } = restored;
        let mut instance = Self::new(session, factory, agent_id_allocator, state);
        instance.restore_agents(agent_parts)?;
        Ok(instance)
    }

    fn restore_agents(
        &mut self,
        pending: BTreeMap<AgentId, Vec<AgentPartState>>,
    ) -> Result<(), OrchestratorInstanceRestoreError> {
        let agents = self
            .state
            .get()
            .graph
            .nodes()
            .map(|(id, meta)| (id, meta.clone()))
            .collect::<Vec<_>>();
        for (id, meta) in agents {
            let placement = if self.state.get().graph.is_root(id) {
                AgentPlacement::Root {
                    session: self.session,
                    persistence: SessionPersistence::Persistent,
                }
            } else {
                AgentPlacement::Sub(id)
            };
            self.build_agent(id, meta.kind(), String::new(), placement, Vec::new())
                .map_err(|source| OrchestratorInstanceRestoreError::agent(id, source))?;

            let parts = pending
                .get(&id)
                .ok_or_else(|| OrchestratorInstanceRestoreError::part_roster(id))?;
            let agent = self
                .registry
                .get_mut(id)
                .ok_or_else(|| OrchestratorInstanceRestoreError::missing_agent(id))?;
            let expected = agent
                .durable_parts()
                .into_iter()
                .map(|part| part.name())
                .collect::<BTreeSet<_>>();
            let actual = parts
                .iter()
                .map(|part| part.name.as_str())
                .collect::<BTreeSet<_>>();
            if expected.len() != parts.len() || expected != actual {
                return Err(OrchestratorInstanceRestoreError::part_roster(id));
            }
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
        inherited_context: Vec<Block<'static>>,
    ) -> Result<(), FsAgentCreateError> {
        let agent = self.factory.create_agent(
            id,
            kind,
            goal,
            placement,
            Arc::clone(&self.host),
            inherited_context,
        )?;
        self.registry.insert(id, agent);
        Ok(())
    }
}
