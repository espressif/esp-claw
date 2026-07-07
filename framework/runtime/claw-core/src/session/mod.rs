//! In-memory session registry and delivery errors.

use std::collections::HashSet;
use std::sync::{Mutex, MutexGuard};

crate::define_prefixed_id!(SessionId, "session-", "session");

crate::define_id_allocator!(
    /// Hands out process-unique [`SessionId`]s for the current runtime.
    SessionIdAllocator(SessionId),
    SessionId(1)
);

// ---------------------------------------------------------------------------
// Session registry
// ---------------------------------------------------------------------------

/// The store's mutable state, guarded by a single lock.
///
/// The session set and the id counter are one logical state, so they live under
/// one `Mutex`: `create` allocates an id and inserts it in a single critical
/// section. The [`SessionIdAllocator`] is the lock-free core — the lock it needs
/// is this outer one, not a second one of its own.
struct Registry {
    sessions: HashSet<SessionId>,
    ids: SessionIdAllocator,
}

pub(crate) struct SessionStore {
    registry: Mutex<Registry>,
}

impl Default for SessionStore {
    fn default() -> Self {
        Self::new()
    }
}

impl SessionStore {
    /// A non-persistent store: the registry lives only in memory and is empty on
    /// every boot.
    pub fn new() -> Self {
        Self {
            registry: Mutex::new(Registry {
                sessions: HashSet::new(),
                ids: SessionIdAllocator::new(),
            }),
        }
    }

    fn lock_registry(&self) -> MutexGuard<'_, Registry> {
        self.registry
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub fn create(&self) -> SessionId {
        let mut registry = self.lock_registry();
        let id = registry.ids.next();
        registry.sessions.insert(id);
        id
    }

    pub fn list(&self) -> Vec<SessionId> {
        self.lock_registry().sessions.iter().copied().collect()
    }

    pub fn delete(&self, session_id: SessionId) -> bool {
        let mut registry = self.lock_registry();
        registry.sessions.remove(&session_id)
    }

    pub fn contains(&self, session_id: SessionId) -> bool {
        self.lock_registry().sessions.contains(&session_id)
    }
}

// ---------------------------------------------------------------------------
#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum DeliverError {
    #[error("session not found: {0}")]
    SessionNotFound(SessionId),
    #[error("agent delivery failed: {0}")]
    Agent(String),
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn session_id_serializes_to_prefixed_string() {
        let value = serde_json::to_value(SessionId(1)).unwrap();
        assert_eq!(value, json!("session-1"));
    }

    #[test]
    fn session_id_deserializes_from_prefixed_string() {
        let session: SessionId = serde_json::from_value(json!("session-7")).unwrap();
        assert_eq!(session, SessionId(7));
    }

    #[test]
    fn session_id_rejects_non_prefixed_wire_values() {
        assert!(serde_json::from_value::<SessionId>(json!("sess-7")).is_err());
        assert!(serde_json::from_value::<SessionId>(json!(7)).is_err());
        assert!(SessionId::from_wire("7").is_err());
    }

    #[test]
    fn session_id_display_matches_wire_format() {
        assert_eq!(SessionId(1).to_string(), "session-1");
    }
}
