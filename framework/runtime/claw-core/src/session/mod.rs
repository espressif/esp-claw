//! In-memory session registry and delivery errors.

mod message;

use std::borrow::Cow;
use std::sync::{Mutex, MutexGuard};

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurablePartSnapshot, DurableState,
    DurableStateCodec, PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use serde::{Deserialize, Serialize};

crate::define_prefixed_id!(SessionId, "session-", "session");
crate::define_prefixed_id!(TurnId, "turn-", "turn");

pub(crate) use message::deserialize_message_or_text;
pub use message::Message;

/// Whether a session survives a runtime restart.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SessionPersistence {
    /// Checkpoint session state and write the root transcript to storage.
    Persistent,
    /// Keep session state and transcript in memory for this process only.
    Ephemeral,
}

crate::define_id_allocator!(
    /// Hands out process-unique [`SessionId`]s for the current runtime.
    SessionIdAllocator(SessionId),
    SessionId(1)
);

crate::define_id_allocator!(
    /// Hands out session-local [`TurnId`]s for one open session.
    pub(crate) TurnIdAllocator(TurnId),
    TurnId(1)
);

// ---------------------------------------------------------------------------
// Session registry
// ---------------------------------------------------------------------------

/// The store's mutable state, guarded by a single lock.
///
/// The session set and next id are one logical state, so they live under one
/// `Mutex`: `create` allocates an id and inserts it in a single critical
/// section.
struct Registry {
    state: DurableState<SessionStoreState>,
    ephemeral_sessions: Vec<SessionId>,
    next_runtime_session_id: SessionId,
}

pub(crate) struct SessionStore {
    registry: Mutex<Registry>,
}

impl SessionStore {
    /// Build a session store from durable state.
    pub(crate) fn new(state: SessionStoreState) -> Self {
        let next_runtime_session_id = state.next_session_id;
        Self {
            registry: Mutex::new(Registry {
                state: DurableState::new(state),
                ephemeral_sessions: Vec::new(),
                next_runtime_session_id,
            }),
        }
    }

    fn lock_registry(&self) -> MutexGuard<'_, Registry> {
        self.registry
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub(crate) fn create(&self, persistence: SessionPersistence) -> SessionId {
        let mut registry = self.lock_registry();
        let id = registry.next_runtime_session_id;
        let next_session_id = SessionId::new(id.0.saturating_add(1));
        registry.next_runtime_session_id = next_session_id;
        match persistence {
            SessionPersistence::Persistent => {
                let state = registry.state.get_mut();
                state.next_session_id = next_session_id;
                state.sessions.push(id);
            }
            SessionPersistence::Ephemeral => registry.ephemeral_sessions.push(id),
        }
        id
    }

    pub(crate) fn list(&self) -> Vec<SessionId> {
        let registry = self.lock_registry();
        let mut sessions = registry.state.get().sessions.clone();
        sessions.extend_from_slice(&registry.ephemeral_sessions);
        sessions.sort_by_key(|session| session.0);
        sessions
    }

    pub(crate) fn delete(&self, session_id: SessionId) -> bool {
        let mut registry = self.lock_registry();
        if let Some(position) = registry
            .state
            .get()
            .sessions
            .iter()
            .position(|session| *session == session_id)
        {
            registry.state.get_mut().sessions.remove(position);
            return true;
        }
        let Some(position) = registry
            .ephemeral_sessions
            .iter()
            .position(|session| *session == session_id)
        else {
            return false;
        };
        registry.ephemeral_sessions.remove(position);
        true
    }

    pub(crate) fn contains(&self, session_id: SessionId) -> bool {
        self.persistence(session_id).is_some()
    }

    pub(crate) fn persistence(&self, session_id: SessionId) -> Option<SessionPersistence> {
        let registry = self.lock_registry();
        if registry.state.get().sessions.contains(&session_id) {
            Some(SessionPersistence::Persistent)
        } else if registry.ephemeral_sessions.contains(&session_id) {
            Some(SessionPersistence::Ephemeral)
        } else {
            None
        }
    }

    pub(crate) fn with_durable_snapshot<T>(
        &self,
        use_snapshot: impl FnOnce(DurablePartSnapshot) -> T,
    ) -> Result<T, DurablePartError> {
        let registry = self.lock_registry();
        let generation = registry.state.generation();
        let state = registry.state.export_state()?.into_owned();
        let snapshot = DurablePartSnapshot::new(
            "session-store",
            generation,
            state,
            StorageHint {
                size: StorageSizeHint::Small,
                change: ChangePatternHint::Arbitrary,
            },
        );
        Ok(use_snapshot(snapshot))
    }
}

impl Default for SessionStoreState {
    fn default() -> Self {
        Self {
            sessions: Vec::new(),
            next_session_id: SessionIdAllocator::new().peek(),
        }
    }
}

impl SessionStoreState {
    fn normalize(&mut self) {
        self.sessions.sort_by_key(|session| session.0);
        self.sessions.dedup();
        if let Some(next) = self
            .sessions
            .iter()
            .map(|session| SessionId::new(session.0.saturating_add(1)))
            .max_by_key(|session| session.0)
        {
            self.next_session_id = SessionId::new(self.next_session_id.0.max(next.0));
        }
    }
}

#[derive(Deserialize, Serialize)]
pub(crate) struct SessionStoreState {
    sessions: Vec<SessionId>,
    next_session_id: SessionId,
}

impl DurableStateCodec for SessionStoreState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        let mut decoded: Self =
            serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)?;
        decoded.normalize();
        Ok(decoded)
    }
}

impl DurablePart for SessionStore {
    fn name(&self) -> &'static str {
        "session-store"
    }

    fn generation(&self) -> PartGeneration {
        self.lock_registry().state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let registry = self.lock_registry();
        let blob = registry.state.export_state()?;
        Ok(PartStateBlob {
            schema_version: blob.schema_version,
            bytes: Cow::Owned(blob.bytes.into_owned()),
        })
    }

    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(Self::new(SessionStoreState::decode_state(state)?))
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ephemeral_sessions_stay_out_of_durable_state() {
        let sessions = SessionStore::new(SessionStoreState::default());
        let ephemeral = sessions.create(SessionPersistence::Ephemeral);
        let persistent = sessions.create(SessionPersistence::Persistent);

        assert_eq!(sessions.list(), vec![ephemeral, persistent]);

        let encoded = sessions.export_state().unwrap();
        let restored = SessionStore::restore_from_state(encoded.as_slice()).unwrap();

        assert_eq!(restored.list(), vec![persistent]);
        assert_eq!(
            restored.persistence(persistent),
            Some(SessionPersistence::Persistent)
        );
        assert_eq!(restored.persistence(ephemeral), None);
        assert_eq!(
            restored.create(SessionPersistence::Ephemeral),
            SessionId::new(persistent.0.saturating_add(1))
        );
    }
}
