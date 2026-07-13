//! Per-session live-agent storage and process-wide agent-id allocation.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use claw_interface::{ClawHttp, ClawTimer};
use serde::{Deserialize, Serialize};

use crate::agent::base_agent::{AgentAbortHandle, AgentId, BaseAgent};

crate::define_id_allocator!(AgentIdCounter(AgentId), AgentId(1));

/// Clones share one process-wide counter.
#[derive(Clone, Debug)]
pub(crate) struct AgentIdAllocator(Arc<Mutex<AgentIdCounter>>);

impl AgentIdAllocator {
    pub(crate) fn new() -> Self {
        Self(Arc::new(Mutex::new(AgentIdCounter::new())))
    }

    fn starting_at(first: AgentId) -> Self {
        Self(Arc::new(Mutex::new(AgentIdCounter::starting_at(first))))
    }

    pub(crate) fn next(&self) -> AgentId {
        self.0
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .next()
    }

    pub(crate) fn peek(&self) -> AgentId {
        self.0.lock().unwrap_or_else(|poison| poison.into_inner()).0
    }
}

impl Serialize for AgentIdAllocator {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.peek().serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for AgentIdAllocator {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let next = AgentId::deserialize(deserializer)?;
        Ok(Self::starting_at(next))
    }
}

/// The per-session agent store: a flat map of agents keyed by [`AgentId`].
pub(crate) struct AgentRegistry<Http: ClawHttp, Timer: ClawTimer> {
    agents: HashMap<AgentId, BaseAgent<Http, Timer>>,
}

impl<Http: ClawHttp, Timer: ClawTimer> AgentRegistry<Http, Timer> {
    pub(crate) fn new() -> Self {
        Self {
            agents: HashMap::new(),
        }
    }

    pub(crate) fn insert(&mut self, id: AgentId, agent: BaseAgent<Http, Timer>) {
        self.agents.insert(id, agent);
    }

    pub(crate) fn get_mut(&mut self, id: AgentId) -> Option<&mut BaseAgent<Http, Timer>> {
        self.agents.get_mut(&id)
    }

    pub(crate) fn remove(&mut self, id: AgentId) -> bool {
        self.agents.remove(&id).is_some()
    }

    pub(crate) fn take(&mut self, id: AgentId) -> Option<BaseAgent<Http, Timer>> {
        self.agents.remove(&id)
    }

    pub(crate) fn iter(&self) -> impl Iterator<Item = (AgentId, &BaseAgent<Http, Timer>)> + '_ {
        self.agents.iter().map(|(&id, agent)| (id, agent))
    }

    /// Handles remain valid while an agent is checked out for a tick.
    pub(crate) fn abort_handles(&self) -> Vec<AgentAbortHandle> {
        self.agents
            .values()
            .map(|agent| agent.abort_handle())
            .collect()
    }
}
