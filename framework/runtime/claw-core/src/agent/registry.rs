//! [`AgentRegistry`] — the per-session agent **store**.
//!
//! The registry owns exactly one thing: the live agents of a session, keyed by
//! [`AgentId`]. It is a dumb map: insert, get a mutable agent, remove, count.
//! Nothing else.
//!
//! Everything *about* the agents — identity allocation, the factory that builds
//! them, the parent/child graph, scheduling, outcome routing, approval handling,
//! and lifecycle policy — lives in the orchestrator instance. The registry knows
//! nothing of edges, depth, readiness, or what a tick outcome means; it only
//! stores what the instance hands it and gives back handles on request.
//!
//! This module also defines the shared [`AgentIdAllocator`] (process-unique ids),
//! used by the instance, not by the store itself: it is colocated here because it
//! is agent-identity infrastructure. Agent construction lives in
//! [`FsAgentFactory`](crate::agent::FsAgentFactory).

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use crate::agent::base_agent::{AgentAbortHandle, AgentId};
use crate::agent::Agent;

crate::define_id_allocator!(
    /// The lock-free core counter behind [`AgentIdAllocator`]. The first
    /// handed-out id is `agent-1` (0 reads like "unset").
    AgentIdCounter(AgentId),
    AgentId(1)
);

/// Hands out process-unique [`AgentId`]s from one shared counter.
///
/// This is the **shared / cloned** allocator case: it is cloned into every
/// per-session instance (see [`OrchestratorInstance`](crate::orchestrator)) and
/// drawn from while a session drives with the orchestrator's map lock released,
/// so there is no common enclosing lock to piggyback on. The synchronization the
/// [`define_id_allocator!`](claw_utils::define_id_allocator) core deliberately
/// omits therefore lives here, at the one shared owner: an `Arc<Mutex<_>>` whose
/// clones all draw from the same counter. Id allocation runs once per spawn, so a
/// `Mutex` is more than sufficient.
#[derive(Clone, Debug, Default)]
pub(crate) struct AgentIdAllocator(Arc<Mutex<AgentIdCounter>>);

impl AgentIdAllocator {
    /// Start a fresh allocator whose first handed-out id is `agent-1`.
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Allocate the next id, advancing the shared counter.
    pub(crate) fn next(&self) -> AgentId {
        self.0
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .next()
    }
}

/// The per-session agent store: a flat map of agents keyed by [`AgentId`].
pub(crate) struct AgentRegistry {
    agents: HashMap<AgentId, Box<dyn Agent>>,
}

impl AgentRegistry {
    /// Create an empty store.
    pub(crate) fn new() -> Self {
        Self {
            agents: HashMap::new(),
        }
    }

    /// Store `agent` under `id`. An existing agent under the same id is replaced
    /// (the instance allocates unique ids, so this does not happen in practice).
    pub(crate) fn insert(&mut self, id: AgentId, agent: Box<dyn Agent>) {
        self.agents.insert(id, agent);
    }

    /// Mutable access to the agent `id`, or `None` if no such agent is stored.
    pub(crate) fn get_mut(&mut self, id: AgentId) -> Option<&mut Box<dyn Agent>> {
        self.agents.get_mut(&id)
    }

    /// Drop the agent `id` from the store. Returns `false` if no such agent
    /// existed.
    pub(crate) fn remove(&mut self, id: AgentId) -> bool {
        self.agents.remove(&id).is_some()
    }

    /// Take ownership of the agent `id` out of the store.
    pub(crate) fn take(&mut self, id: AgentId) -> Option<Box<dyn Agent>> {
        self.agents.remove(&id)
    }

    /// An abort handle for every currently-stored agent.
    ///
    /// The instance captures these in a batch-local cancel hook so an
    /// out-of-band cancel can abort whatever agents are live. Handles are
    /// `Arc`-backed clones, so they stay valid even after an agent is
    /// [`take`](Self::take)n out for a tick.
    pub(crate) fn abort_handles(&self) -> Vec<AgentAbortHandle> {
        self.agents
            .values()
            .map(|agent| agent.abort_handle())
            .collect()
    }

    /// The number of live agents in the store. Used by the instance's test-only
    /// `agent_count`; a non-test caller arrives with Part B's resource caps.
    #[cfg(test)]
    pub(crate) fn count(&self) -> usize {
        self.agents.len()
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;
    use crate::agent::base_agent::{
        AgentAbortHandle, AgentCommand, AgentCommandError, TickOutcome,
    };
    use crate::agent::AgentTickFuture;

    /// A trivial agent: does nothing, always idle. Enough to exercise the store.
    struct NoopAgent {
        id: AgentId,
    }

    impl Agent for NoopAgent {
        fn id(&self) -> AgentId {
            self.id
        }
        fn send_command(&mut self, _command: AgentCommand) -> Result<(), AgentCommandError> {
            Ok(())
        }
        fn deliver_child_result(&mut self, _child: AgentId, _text: String, _ok: bool) {}
        fn deliver_child_input(&mut self, _child: AgentId, _text: String) {}
        fn abort_handle(&self) -> AgentAbortHandle {
            AgentAbortHandle::default()
        }
        fn tick(&mut self) -> AgentTickFuture<'_> {
            Box::pin(async { TickOutcome::Idle })
        }
    }

    #[test]
    fn insert_get_remove_count() {
        let mut registry = AgentRegistry::new();
        assert_eq!(registry.count(), 0);

        let id = AgentId(1);
        registry.insert(id, Box::new(NoopAgent { id }));
        assert_eq!(registry.count(), 1);
        assert!(registry.get_mut(id).is_some());
        assert_eq!(registry.get_mut(id).unwrap().id(), id);

        assert!(registry.remove(id));
        assert!(!registry.remove(id));
        assert!(registry.get_mut(id).is_none());
        assert_eq!(registry.count(), 0);
    }

    #[test]
    fn allocator_hands_out_unique_ascending_ids() {
        let allocator = AgentIdAllocator::new();
        let a = allocator.next();
        let b = allocator.next();
        // A clone shares the same counter, so ids stay globally unique.
        let c = allocator.clone().next();
        assert!(a.0 < b.0 && b.0 < c.0);
    }
}
