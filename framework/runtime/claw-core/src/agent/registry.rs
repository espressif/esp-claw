//! [`AgentRegistry`] — the per-session agent **store**.
//!
//! The registry owns exactly one thing: the live agents of a session, keyed by
//! [`AgentId`], each behind an `Arc<Mutex<_>>` handle so the scheduler can drive
//! them (and, later, tick several concurrently). It is a dumb map: insert, get a
//! handle, remove, count. Nothing else.
//!
//! Everything *about* the agents — identity allocation, the factory that builds
//! them, the parent/child graph, scheduling, outcome routing, approval handling,
//! and lifecycle policy — lives in the orchestrator instance. The registry knows
//! nothing of edges, depth, readiness, or what a tick outcome means; it only
//! stores what the instance hands it and gives back handles on request.
//!
//! This module also defines the shared [`AgentIdAllocator`] (process-unique ids),
//! used by the instance, not by the store itself: it is colocated here because it
//! is agent-identity infrastructure. The companion construction seam — how an
//! agent of a kind is built — is [`AgentFactory`](crate::agent::factory::AgentFactory).

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use crate::agent::base_agent::AgentId;
use crate::agent::Agent;

/// The first [`AgentId`] handed out (0 reads like "unset").
const FIRST_AGENT_ID: AgentId = AgentId(1);

/// A live agent behind a shared, lockable handle.
///
/// The instance obtains a handle by id and locks it to tick or command the agent;
/// the registry keeps an equal handle as the agent's owner of record (dropped on
/// [`remove`](AgentRegistry::remove)). The `Mutex` satisfies the borrow checker
/// for one-at-a-time access today and is the seam for concurrent async ticking
/// later (each future locks only its own agent).
pub(crate) type AgentHandle = Arc<Mutex<Box<dyn Agent>>>;

/// Hands out process-unique [`AgentId`]s from a shared counter.
///
/// The counter is the **next** [`AgentId`] to hand out, stored as an `AgentId`
/// (not a bare integer) so the allocator follows [`AgentId`]'s representation if
/// it ever changes — there is no independent `usize` assumption here. Cloning
/// shares the same counter, so several per-session instances draw
/// globally-unique ids from one allocator. id allocation is not a hot path (once
/// per spawn / per session root), so a `Mutex` is sufficient.
#[derive(Clone, Debug)]
pub(crate) struct AgentIdAllocator(Arc<Mutex<AgentId>>);

impl Default for AgentIdAllocator {
    fn default() -> Self {
        Self::new()
    }
}

impl AgentIdAllocator {
    /// Start a fresh allocator whose first handed-out id is `agent-1`.
    pub(crate) fn new() -> Self {
        Self(Arc::new(Mutex::new(FIRST_AGENT_ID)))
    }

    /// Allocate the next process-unique [`AgentId`].
    pub(crate) fn next(&self) -> AgentId {
        let mut next = self.0.lock().unwrap_or_else(|poison| poison.into_inner());
        let id = *next;
        *next = AgentId(id.0.saturating_add(1));
        id
    }
}

/// The per-session agent store: a flat map of agents keyed by [`AgentId`].
pub(crate) struct AgentRegistry {
    agents: HashMap<AgentId, AgentHandle>,
}

impl AgentRegistry {
    /// Create an empty store.
    pub(crate) fn new() -> Self {
        Self {
            agents: HashMap::new(),
        }
    }

    /// Store `agent` under `id`, returning its handle. An existing agent under the
    /// same id is replaced (the instance allocates unique ids, so this does not
    /// happen in practice).
    pub(crate) fn insert(&mut self, id: AgentId, agent: Box<dyn Agent>) -> AgentHandle {
        let handle: AgentHandle = Arc::new(Mutex::new(agent));
        self.agents.insert(id, Arc::clone(&handle));
        handle
    }

    /// A handle to the agent `id`, or `None` if no such agent is stored. The
    /// returned `Arc` clone holds no borrow of the store, so callers may obtain
    /// several handles and lock each independently.
    pub(crate) fn get(&self, id: AgentId) -> Option<AgentHandle> {
        self.agents.get(&id).map(Arc::clone)
    }

    /// Drop the agent `id` from the store. Returns `false` if no such agent
    /// existed.
    pub(crate) fn remove(&mut self, id: AgentId) -> bool {
        self.agents.remove(&id).is_some()
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
    use crate::agent::base_agent::{AgentCommand, AgentCommandError, TickOutcome};

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
        fn tick(&mut self) -> TickOutcome {
            TickOutcome::Idle
        }
    }

    #[test]
    fn insert_get_remove_count() {
        let mut registry = AgentRegistry::new();
        assert_eq!(registry.count(), 0);

        let id = AgentId(1);
        let handle = registry.insert(id, Box::new(NoopAgent { id }));
        assert_eq!(registry.count(), 1);
        assert!(registry.get(id).is_some());
        assert_eq!(handle.lock().unwrap().id(), id);

        assert!(registry.remove(id));
        assert!(!registry.remove(id));
        assert!(registry.get(id).is_none());
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
