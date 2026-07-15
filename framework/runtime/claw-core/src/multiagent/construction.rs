use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;
use std::sync::Arc;

use claw_checkpoint::{DurableState, PartStateSlice};
use claw_context::Block;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_permission::PermissionPolicy;

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
        permission_policy: Arc<dyn PermissionPolicy>,
        state: MultiagentState,
    ) -> Self {
        let multiagent = Arc::new(MultiagentBridge::new(agent_id_allocator.clone()));
        Self {
            session,
            factory,
            permission_policy,
            agent_id_allocator,
            state: DurableState::new(state),
            slots: AgentSlots::new(),
            timeouts: Default::default(),
            foreground_results: BTreeMap::new(),
            pending_deliveries: Default::default(),
            multiagent,
        }
    }

    pub(crate) fn from_restored_state(
        session: SessionId,
        factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        permission_policy: Arc<dyn PermissionPolicy>,
        restored: MultiagentRestore,
    ) -> Result<Self, MultiagentRestoreError> {
        let MultiagentRestore { state, agent_slots } = restored;
        let mut instance = Self::new(
            session,
            factory,
            agent_id_allocator,
            permission_policy,
            state,
        );
        instance.restore_agents(agent_slots)?;
        instance.rearm_subagent_timeouts();
        Ok(instance)
    }

    fn rearm_subagent_timeouts(&mut self) {
        let pending = self
            .state
            .get()
            .nodes()
            .filter_map(|(id, meta)| meta.timeout().map(|timeout| (id, timeout)))
            .collect::<Vec<_>>();
        for (id, timeout) in pending {
            self.timeouts.arm::<Timer>(id, timeout);
        }
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
        let environment = AgentEnvironment::new(
            transcript,
            profile,
            Arc::clone(&self.permission_policy),
            extension_tools,
            inherited_context,
        );
        let agent = self.factory.create_agent(id, kind, goal, environment)?;
        self.slots.insert(id, agent);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::rc::Rc;
    use std::sync::{Arc, RwLock};

    use claw_checkpoint::{DurablePart, PartStateSlice};
    use claw_interface::{ImmediateTimer, MemFs, RealHttp};
    use claw_permission::AllowAll;
    use claw_tool::ToolRegistry;
    use futures_lite::future::block_on;

    use crate::agent::FsAgentFactory;
    use crate::config::ClawApiManager;
    use crate::protocol::{AgentId, AgentKind, Message, SessionId, SessionPersistence};

    use super::super::model::SubagentTimeout;
    use super::super::{
        AgentIdAllocator, AgentPlacement, MultiagentRestore, MultiagentRuntime, MultiagentState,
        ROOT_AGENT_KIND,
    };

    type TestRuntime = MultiagentRuntime<MemFs, RealHttp, ImmediateTimer>;

    #[test]
    #[allow(clippy::arc_with_non_send_sync)]
    fn restore_rearms_each_persisted_subagent_with_its_full_timeout() {
        MemFs::new();
        let session = SessionId::new(1);
        let factory = Rc::new(
            FsAgentFactory::new(
                Arc::new(ToolRegistry::new()),
                "/timeout-restore-test".to_owned(),
                Vec::new(),
                Arc::new(RwLock::new(ClawApiManager::new())),
            )
            .expect("test factory builds"),
        );
        let mut runtime = TestRuntime::new(
            session,
            Rc::clone(&factory),
            AgentIdAllocator::new(),
            Arc::new(AllowAll),
            MultiagentState::default(),
        );
        let root = AgentId::new(1);
        let child = AgentId::new(2);
        let root_kind = AgentKind::from_static(ROOT_AGENT_KIND);
        let child_kind = AgentKind::from_static("worker");
        runtime
            .build_agent(
                root,
                &root_kind,
                Message::text("root"),
                AgentPlacement::Root {
                    session,
                    persistence: SessionPersistence::Persistent,
                },
                Vec::new(),
            )
            .expect("root builds");
        assert!(runtime.state.get_mut().insert_root(root, root_kind));
        runtime
            .build_agent(
                child,
                &child_kind,
                Message::text("child"),
                AgentPlacement::Child(child),
                Vec::new(),
            )
            .expect("child builds");
        let timeout = SubagentTimeout::from_millis(12_345).expect("non-zero timeout");
        assert!(runtime.state.get_mut().insert_child(
            root,
            child,
            child_kind,
            Some("restored-child".to_owned()),
            timeout,
        ));

        let checkpoint = runtime.export_state().expect("runtime checkpoints");
        let restored = MultiagentRestore::decode_state(PartStateSlice {
            schema_version: checkpoint.schema_version,
            bytes: checkpoint.bytes.as_ref(),
        })
        .expect("checkpoint decodes");
        let mut runtime = TestRuntime::from_restored_state(
            session,
            factory,
            AgentIdAllocator::new(),
            Arc::new(AllowAll),
            restored,
        )
        .expect("runtime restores");

        let expired = block_on(runtime.timeouts.next_expired());
        assert_eq!(expired.len(), 1);
        let expired = expired.first().expect("one restored timeout");
        assert_eq!(expired.agent, child);
        assert_eq!(expired.timeout.millis(), 12_345);
    }
}
