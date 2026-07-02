//! Session registry, session-scoped message types, and deliver errors.

use std::collections::HashMap;
use std::sync::{Mutex, MutexGuard};

use thiserror::Error;

crate::define_prefixed_id!(SessionId, "session-", "session");

// ---------------------------------------------------------------------------
// Session registry
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionRecord {
    pub id: SessionId,
    pub channel: Option<String>,
    pub chat_id: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum SessionError {
    #[error("session not found: {0}")]
    NotFound(SessionId),
    #[error("session already exists: {0}")]
    AlreadyExists(SessionId),
}

/// In-memory session registry (device persistence lives in C layer later).
#[derive(Default)]
pub struct SessionStore {
    sessions: Mutex<HashMap<SessionId, SessionRecord>>,
    next_id: Mutex<usize>,
}

impl SessionStore {
    pub fn new() -> Self {
        Self::default()
    }

    fn lock_sessions(&self) -> MutexGuard<'_, HashMap<SessionId, SessionRecord>> {
        self.sessions.lock().unwrap()
    }

    fn alloc_id(&self) -> SessionId {
        let mut next = self.next_id.lock().unwrap();
        *next += 1;
        SessionId(*next)
    }

    pub fn create(&self) -> SessionRecord {
        let id = self.alloc_id();
        let record = SessionRecord {
            id,
            channel: None,
            chat_id: None,
        };
        self.lock_sessions().insert(id, record.clone());
        record
    }

    pub fn create_with_id(&self, id: SessionId) -> Result<SessionRecord, SessionError> {
        let mut sessions = self.lock_sessions();
        if sessions.contains_key(&id) {
            return Err(SessionError::AlreadyExists(id));
        }
        let record = SessionRecord {
            id,
            channel: None,
            chat_id: None,
        };
        sessions.insert(id, record.clone());
        Ok(record)
    }

    pub fn list(&self) -> Vec<SessionRecord> {
        self.lock_sessions().values().cloned().collect()
    }

    pub fn count(&self) -> usize {
        self.lock_sessions().len()
    }

    pub fn delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        self.lock_sessions()
            .remove(&session_id)
            .map(|_| ())
            .ok_or(SessionError::NotFound(session_id))
    }

    pub fn get(&self, session_id: SessionId) -> Option<SessionRecord> {
        self.lock_sessions().get(&session_id).cloned()
    }

    pub fn contains(&self, session_id: SessionId) -> bool {
        self.lock_sessions().contains_key(&session_id)
    }
}

// ---------------------------------------------------------------------------
// Session message types
// ---------------------------------------------------------------------------

/// User message payload passed to orchestrator callbacks (no session id).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionMessage {
    pub text: String,
    pub message_id: String,
    pub sender_id: Option<String>,
}

impl SessionMessage {
    pub fn new(
        text: impl Into<String>,
        message_id: impl Into<String>,
        sender_id: Option<String>,
    ) -> Self {
        Self {
            text: text.into(),
            message_id: message_id.into(),
            sender_id,
        }
    }
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum DeliverError {
    #[error("session not found: {0}")]
    SessionNotFound(SessionId),
    #[error("agent delivery failed: {0}")]
    Agent(String),
    #[error(transparent)]
    Session(#[from] SessionError),
}

#[cfg(test)]
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
