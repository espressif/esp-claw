//! In-memory session registry and delivery errors.

mod message;

use std::borrow::Cow;
use std::sync::{Mutex, MutexGuard};

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use serde::{Deserialize, Serialize};

crate::define_prefixed_id!(SessionId, "session-", "session");
crate::define_prefixed_id!(TurnId, "turn-", "turn");

pub use message::{AttachmentId, AttachmentKind, AttachmentRecord, AttachmentRef, Message};

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
}

pub(crate) struct SessionStore {
    registry: Mutex<Registry>,
}

impl Default for SessionStore {
    fn default() -> Self {
        Self::new(SessionStoreState::default())
    }
}

impl SessionStore {
    /// Build a session store from durable state.
    pub(crate) fn new(state: SessionStoreState) -> Self {
        Self {
            registry: Mutex::new(Registry {
                state: DurableState::new(state),
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
        let state = registry.state.get_mut();
        let id = state.next_session_id;
        state.next_session_id = SessionId::new(id.0.saturating_add(1));
        state.sessions.push(id);
        id
    }

    pub fn list(&self) -> Vec<SessionId> {
        self.lock_registry().state.get().sessions.clone()
    }

    pub fn delete(&self, session_id: SessionId) -> bool {
        let mut registry = self.lock_registry();
        let Some(position) = registry
            .state
            .get()
            .sessions
            .iter()
            .position(|session| *session == session_id)
        else {
            return false;
        };
        registry.state.get_mut().sessions.remove(position);
        true
    }

    pub fn contains(&self, session_id: SessionId) -> bool {
        self.lock_registry()
            .state
            .get()
            .sessions
            .contains(&session_id)
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
