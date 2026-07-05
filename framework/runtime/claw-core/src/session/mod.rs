//! In-memory session registry, delivery modes, and delivery errors.

use std::collections::HashSet;
use std::sync::{Mutex, MutexGuard};

use thiserror::Error;

crate::define_prefixed_id!(SessionId, "session-", "session");

crate::define_id_allocator!(
    /// Hands out process-unique [`SessionId`]s for the current runtime.
    SessionIdAllocator(SessionId),
    SessionId(1)
);

// ---------------------------------------------------------------------------
// Session registry
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum SessionError {
    #[error("session not found: {0}")]
    NotFound(SessionId),
}

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

    pub fn delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        let mut registry = self.lock_registry();
        if !registry.sessions.remove(&session_id) {
            return Err(SessionError::NotFound(session_id));
        }
        Ok(())
    }

    pub fn contains(&self, session_id: SessionId) -> bool {
        self.lock_registry().sessions.contains(&session_id)
    }
}

// ---------------------------------------------------------------------------
// Session message types
// ---------------------------------------------------------------------------

/// How a delivered message should interact with any task already running in the
/// target session.
///
/// This is a **core orchestration** concept, not a transport one: it describes
/// how the driving layer treats an in-flight task, so it lives in `claw-core`
/// beside [`Orchestrator`](crate::orchestrator::Orchestrator). Transports never
/// name it. A transport message may carry an opaque hint, which is resolved into
/// a `DeliveryKind` at the transport/session boundary.
///
/// See [`Orchestrator::submit`](crate::orchestrator::Orchestrator::submit) for
/// how `Append`/`Interrupt`/`Cancel` are sequenced against an in-flight drive.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum DeliveryKind {
    /// Add the message as ordinary input at the next delivery boundary. When the
    /// session is idle this starts a fresh task; if a drive is already in flight,
    /// the concurrent driver defers the append until that drive settles.
    #[default]
    Append,
    /// Graceful, whole-iteration interruption: let the current iteration finish
    /// and commit, then stop the drive. The continuation is delivered through
    /// the root agent's interrupt command, which records an interruption marker
    /// and keeps the task alive.
    Interrupt,
    /// Hard interruption: abort the in-flight LLM round (discarding its partial
    /// result), terminate the current task without committing its open turn, then
    /// start a fresh task from this message.
    Cancel,
}

impl DeliveryKind {
    /// The recognized `extra_context` hint tokens. `Append` is the default and
    /// needs no token (a missing hint means "append").
    const HINT_INTERRUPT: &str = "interrupt";
    const HINT_CANCEL: &str = "cancel";

    /// Resolve a transport message's optional `extra_context` hint into a
    /// `DeliveryKind` at the transport→session boundary.
    ///
    /// `extra_context` is a broader, opaque transport hint; the delivery kind is
    /// only one thing inferred from it. A missing hint (`None`) or one that does
    /// not name a known delivery mode falls back to [`Append`](Self::Append) — the
    /// safe "queue and let the agent decide" default — and is traced so a
    /// malformed hint stays visible. (This fallback is intentional and requested;
    /// it is not a silent config substitution.)
    pub fn from_extra_context(extra_context: Option<&str>) -> Self {
        let Some(hint) = extra_context.map(str::trim) else {
            tracing::trace!("inbound message carried no extra_context; delivering as Append");
            return Self::Append;
        };
        if hint == Self::HINT_INTERRUPT {
            Self::Interrupt
        } else if hint == Self::HINT_CANCEL {
            Self::Cancel
        } else {
            tracing::trace!(
                extra_context = hint,
                "extra_context names no known delivery mode; delivering as Append"
            );
            Self::Append
        }
    }
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum DeliverError {
    #[error("session not found: {0}")]
    SessionNotFound(SessionId),
    #[error("session submission superseded by a newer control message")]
    Superseded,
    #[error("agent delivery failed: {0}")]
    Agent(String),
    #[error(transparent)]
    Session(#[from] SessionError),
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
