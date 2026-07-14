use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;
use std::sync::Arc;

use claw_checkpoint::{DurableState, PartStateSlice};
use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    AgentEnvironment, FsAgentCreateError, FsAgentFactory, ProfileAccess, TranscriptTarget,
};
use crate::protocol::{AgentId, AgentKind, Message, SessionId, SessionPersistence};

use super::persistence::{MultiagentRestore, MultiagentRestoreError, RestoredAgentSlot};
use super::{
    tools, AgentIdAllocator, AgentPlacement, AgentSlots, MultiagentBridge, MultiagentRuntime,
    MultiagentState,
};

impl<Filesystem, Http, Timer> MultiagentRuntime<Filesystem, Http, Timer>
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
        state: MultiagentState,
    ) -> Self {
        let multiagent = Arc::new(MultiagentBridge::new(agent_id_allocator.clone()));
        Self {
            session,
            factory,
            agent_id_allocator,
            state: DurableState::new(state),
            slots: AgentSlots::new(),
            foreground_results: BTreeMap::new(),
            multiagent,
        }
    }

    pub(crate) fn from_restored_state(
        session: SessionId,
        factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        restored: MultiagentRestore,
    ) -> Result<Self, MultiagentRestoreError> {
        let MultiagentRestore { state, agent_slots } = restored;
        let mut instance = Self::new(session, factory, agent_id_allocator, state);
        instance.restore_agents(agent_slots)?;
        Ok(instance)
    }

    fn restore_agents(
        &mut self,
        mut pending: BTreeMap<AgentId, RestoredAgentSlot>,
    ) -> Result<(), MultiagentRestoreError> {
        let agents = self
            .state
            .get()
            .nodes()
            .map(|(id, meta)| (id, meta.clone()))
            .collect::<Vec<_>>();
        for (id, meta) in agents {
            let restored_slot = pending
                .remove(&id)
                .ok_or_else(|| MultiagentRestoreError::part_roster(id))?;
            let placement = if self.state.get().is_root(id) {
                AgentPlacement::Root {
                    session: self.session,
                    persistence: SessionPersistence::Persistent,
                }
            } else {
                AgentPlacement::Child(id)
            };
            self.build_agent(id, meta.kind(), Message::text(""), placement, Vec::new())
                .map_err(|source| MultiagentRestoreError::agent(id, source))?;

            let parts = &restored_slot.parts;
            let agent = self
                .slots
                .available_agent_mut(id)
                .ok_or_else(|| MultiagentRestoreError::missing_agent(id))?;
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
                return Err(MultiagentRestoreError::part_roster(id));
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
                        MultiagentRestoreError::durable_part(id, part.name.clone(), source)
                    })?;
                if !restored {
                    return Err(MultiagentRestoreError::unknown_part(id, part.name.clone()));
                }
            }
            self.slots.restore_inbox(id, restored_slot.inbox);
        }
        Ok(())
    }

    pub(super) fn build_agent(
        &mut self,
        id: AgentId,
        kind: &AgentKind,
        goal: Message,
        placement: AgentPlacement,
        inherited_context: Vec<Block<'static>>,
    ) -> Result<(), FsAgentCreateError> {
        let extension_tools = tools::tool_group(id, kind, Arc::clone(&self.multiagent))
            .into_iter()
            .collect();
        let (transcript, profile) = match placement {
            AgentPlacement::Root {
                session,
                persistence,
            } => {
                let transcript = match persistence {
                    SessionPersistence::Persistent => TranscriptTarget::Persistent(session.0),
                    SessionPersistence::Ephemeral => TranscriptTarget::InMemory(session.0),
                };
                (transcript, ProfileAccess::Writable)
            }
            AgentPlacement::Child(child) => {
                (TranscriptTarget::InMemory(child.0), ProfileAccess::ReadOnly)
            }
        };
        let environment =
            AgentEnvironment::new(transcript, profile, extension_tools, inherited_context);
        let agent = self.factory.create_agent(id, kind, goal, environment)?;
        self.slots.insert(id, agent);
        Ok(())
    }
}
