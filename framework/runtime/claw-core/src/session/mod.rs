//! Session registry, session-scoped message types, and deliver errors.

use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

use claw_interface::{ClawFs, FsError};
use serde::de::{self, Deserializer};
use serde::{Deserialize, Serialize};
use thiserror::Error;

crate::define_prefixed_id!(SessionId, "session-", "session");

crate::define_id_allocator!(
    /// Hands out process-unique [`SessionId`]s, resuming past the highest id
    /// restored from persistence so a new session never reuses one.
    SessionIdAllocator(SessionId),
    SessionId(1)
);

/// Filename of the persisted session registry under the sessions root.
const REGISTRY_FILE: &str = "registry.json";

// ---------------------------------------------------------------------------
// Session registry
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionBinding {
    pub channel: String,
    pub chat_id: String,
}

impl SessionBinding {
    pub fn new(channel: impl Into<String>, chat_id: impl Into<String>) -> Self {
        Self {
            channel: channel.into(),
            chat_id: chat_id.into(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct SessionRecord {
    pub id: SessionId,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub binding: Option<SessionBinding>,
}

#[derive(Deserialize)]
struct SessionRecordWire {
    id: SessionId,
    #[serde(default)]
    binding: Option<SessionBinding>,
    #[serde(default)]
    channel: Option<String>,
    #[serde(default)]
    chat_id: Option<String>,
}

impl<'de> Deserialize<'de> for SessionRecord {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = SessionRecordWire::deserialize(deserializer)?;
        let legacy_binding = match (wire.channel, wire.chat_id) {
            (Some(channel), Some(chat_id)) => Some(SessionBinding::new(channel, chat_id)),
            (None, None) => None,
            _ => {
                return Err(de::Error::custom(
                    "session registry has a partial legacy channel binding",
                ));
            }
        };
        Ok(Self {
            id: wire.id,
            binding: wire.binding.or(legacy_binding),
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum SessionError {
    #[error("session not found: {0}")]
    NotFound(SessionId),
    #[error("session already exists: {0}")]
    AlreadyExists(SessionId),
}

// ---------------------------------------------------------------------------
// Persistence seam
// ---------------------------------------------------------------------------

/// The persistence seam behind the [`SessionStore`]: load the registry at boot
/// and persist a whole-registry snapshot after each mutation.
///
/// Kept as an injected `Arc<dyn ...>` so `claw-core` stays filesystem-agnostic
/// (mirroring [`TranscriptStore`](claw_memory::TranscriptStore) /
/// [`LongTermMemory`](claw_memory::LongTermMemory)); the concrete
/// [`FsSessionRegistry`] over a [`ClawFs`] is wired at the composition root.
///
/// Both methods surface their error so the [`SessionStore`] can log a lost write
/// rather than silently dropping it; the store keeps its in-memory state
/// authoritative regardless.
pub trait SessionRegistryStore: Send + Sync {
    /// Load the persisted records, or an empty set when nothing is stored yet.
    fn load(&self) -> Result<Vec<SessionRecord>, FsError>;

    /// Durably replace the registry with `records`.
    fn persist(&self, records: &[SessionRecord]) -> Result<(), FsError>;
}

/// A [`SessionRegistryStore`] backed by a [`ClawFs`], persisting the whole
/// registry as a single atomically-rewritten `registry.json` under the sessions
/// root.
///
/// The registry is tiny (a handful of records) and read/written whole, so a
/// snapshot rewrite is simpler and cheaper than an append-only journal.
pub struct FsSessionRegistry<F: ClawFs + 'static> {
    path: String,
    fs: F,
}

impl<F: ClawFs + 'static> FsSessionRegistry<F> {
    /// Build a registry store writing `registry.json` under `dir` (the sessions
    /// root). Best-effort creates `dir`.
    pub fn new(dir: &str, fs: F) -> Self {
        if let Err(error) = fs.create_dir_all(dir) {
            tracing::warn!(%dir, %error, "session registry: create dir failed");
        }
        Self {
            path: format!("{}/{}", dir.trim_end_matches('/'), REGISTRY_FILE),
            fs,
        }
    }
}

impl<F: ClawFs + 'static> SessionRegistryStore for FsSessionRegistry<F> {
    fn load(&self) -> Result<Vec<SessionRecord>, FsError> {
        match self.fs.read(&self.path) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map_err(|error| FsError::Io(format!("parse session registry: {error}"))),
            // A missing registry is a fresh device, not an error.
            Err(FsError::NotFound) => Ok(Vec::new()),
            Err(error) => Err(error),
        }
    }

    fn persist(&self, records: &[SessionRecord]) -> Result<(), FsError> {
        let bytes = serde_json::to_vec(records)
            .map_err(|error| FsError::Io(format!("serialize session registry: {error}")))?;
        self.fs.write_atomic(&self.path, &bytes)
    }
}

/// In-memory session registry with an optional durable [`SessionRegistryStore`].
///
/// The in-memory map is authoritative for the running process; every mutation
/// writes a snapshot through the injected store when one is configured, so a
/// restart can rehydrate the set (see [`SessionStore::with_persistence`]).
/// The store's mutable state, guarded by a single lock.
///
/// The session map and the id counter are one logical state, so they live under
/// one `Mutex`: `create` allocates an id and inserts the record in a single
/// critical section. The [`SessionIdAllocator`] is the lock-free core — the lock
/// it needs is this outer one, not a second one of its own.
struct Registry {
    sessions: HashMap<SessionId, SessionRecord>,
    ids: SessionIdAllocator,
}

pub struct SessionStore {
    registry: Mutex<Registry>,
    persistence: Option<Arc<dyn SessionRegistryStore>>,
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
                sessions: HashMap::new(),
                ids: SessionIdAllocator::new(),
            }),
            persistence: None,
        }
    }

    /// A store fronting `persistence`, rehydrated from whatever it has stored.
    ///
    /// The id allocator resumes past the highest restored id so a new session
    /// never reuses a persisted one. A failed [`load`](SessionRegistryStore::load)
    /// is logged and the store starts empty — a corrupt registry must not brick
    /// boot.
    pub fn with_persistence(persistence: Arc<dyn SessionRegistryStore>) -> Self {
        let restored = persistence.load().unwrap_or_else(|error| {
            tracing::warn!(%error, "session registry: load failed; starting empty");
            Vec::new()
        });
        // Resume the allocator one past the highest restored id so `create` never
        // reuses a persisted one. `.0` (not the newtype) is used only because the
        // id newtype does not derive `Ord`, and finding the max needs a compare.
        let highest = restored.iter().map(|record| record.id.0).max().unwrap_or(0);
        let sessions = restored
            .into_iter()
            .map(|record| (record.id, record))
            .collect();
        Self {
            registry: Mutex::new(Registry {
                sessions,
                ids: SessionIdAllocator::starting_at(SessionId(highest.saturating_add(1))),
            }),
            persistence: Some(persistence),
        }
    }

    fn lock_registry(&self) -> MutexGuard<'_, Registry> {
        self.registry
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Snapshot the current records and persist them, logging (not failing) on a
    /// write error so a lost write is observable but the live state stays
    /// authoritative.
    fn persist_snapshot(&self, sessions: &HashMap<SessionId, SessionRecord>) {
        let Some(persistence) = &self.persistence else {
            return;
        };
        let records: Vec<SessionRecord> = sessions.values().cloned().collect();
        if let Err(error) = persistence.persist(&records) {
            tracing::warn!(%error, "session registry: persist failed");
        }
    }

    pub fn create(&self) -> SessionRecord {
        let mut registry = self.lock_registry();
        let id = registry.ids.next();
        let record = SessionRecord { id, binding: None };
        registry.sessions.insert(id, record.clone());
        self.persist_snapshot(&registry.sessions);
        record
    }

    pub fn create_with_id(&self, id: SessionId) -> Result<SessionRecord, SessionError> {
        let mut registry = self.lock_registry();
        if registry.sessions.contains_key(&id) {
            return Err(SessionError::AlreadyExists(id));
        }
        let record = SessionRecord { id, binding: None };
        registry.sessions.insert(id, record.clone());
        self.persist_snapshot(&registry.sessions);
        Ok(record)
    }

    /// Record `session_id`'s channel binding and persist it, so the channel
    /// router can rebuild the `(channel, chat) -> session` route after a restart.
    ///
    /// # Errors
    ///
    /// [`SessionError::NotFound`] when `session_id` is not registered.
    pub fn set_binding(
        &self,
        session_id: SessionId,
        channel: impl Into<String>,
        chat_id: impl Into<String>,
    ) -> Result<(), SessionError> {
        let mut registry = self.lock_registry();
        let record = registry
            .sessions
            .get_mut(&session_id)
            .ok_or(SessionError::NotFound(session_id))?;
        record.binding = Some(SessionBinding::new(channel, chat_id));
        self.persist_snapshot(&registry.sessions);
        Ok(())
    }

    pub fn list(&self) -> Vec<SessionRecord> {
        self.lock_registry().sessions.values().cloned().collect()
    }

    pub fn count(&self) -> usize {
        self.lock_registry().sessions.len()
    }

    pub fn delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        let mut registry = self.lock_registry();
        if registry.sessions.remove(&session_id).is_none() {
            return Err(SessionError::NotFound(session_id));
        }
        self.persist_snapshot(&registry.sessions);
        Ok(())
    }

    pub fn get(&self, session_id: SessionId) -> Option<SessionRecord> {
        self.lock_registry().sessions.get(&session_id).cloned()
    }

    pub fn contains(&self, session_id: SessionId) -> bool {
        self.lock_registry().sessions.contains_key(&session_id)
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
/// name it — an [`InboundMessage`](claw_capability::InboundMessage) carries only
/// an opaque `extra_context` hint, which is resolved into a `DeliveryKind` at the
/// transport→session boundary via [`from_extra_context`](Self::from_extra_context).
///
/// See [`Orchestrator::deliver_interruptible`](crate::orchestrator::Orchestrator::deliver_interruptible)
/// for how `Interrupt`/`Cancel` reach an in-flight drive.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum DeliveryKind {
    /// Add the message as information and let the agent re-decide. When the
    /// session is idle this starts a fresh task; while it is running the message
    /// joins the in-progress task and is folded into the next iteration.
    #[default]
    Append,
    /// Graceful, whole-iteration interruption: let the current iteration finish
    /// and commit, then pause the drive, record an interruption marker, deliver
    /// this message, and keep the task alive.
    Interrupt,
    /// Hard interruption: abort the in-flight LLM round (discarding its partial
    /// result), terminate the current task with a cancellation marker, then start
    /// a fresh task from this message.
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

/// User message payload passed to orchestrator callbacks (no session id).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionMessage {
    pub text: String,
    pub message_id: String,
    pub sender_id: Option<String>,
    /// How this message interacts with an already-running task. Defaults to
    /// [`DeliveryKind::Append`].
    pub kind: DeliveryKind,
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
            kind: DeliveryKind::Append,
        }
    }

    /// Set the [`DeliveryKind`] (builder-style; defaults to
    /// [`DeliveryKind::Append`]).
    pub fn with_kind(mut self, kind: DeliveryKind) -> Self {
        self.kind = kind;
        self
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
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use claw_interface::MemFs;
    use serde_json::json;

    fn record(id: u32) -> SessionRecord {
        SessionRecord {
            id: SessionId(id),
            binding: None,
        }
    }

    #[test]
    fn fs_registry_round_trips_records() {
        let fs = MemFs::default();
        let records = vec![
            record(1),
            SessionRecord {
                id: SessionId(2),
                binding: Some(SessionBinding::new("local", "chat")),
            },
        ];
        FsSessionRegistry::new("/sessions", fs.clone())
            .persist(&records)
            .unwrap();
        // A missing registry loads as empty; a written one round-trips verbatim.
        let loaded = FsSessionRegistry::new("/sessions", fs).load().unwrap();
        assert_eq!(loaded, records);
    }

    #[test]
    fn missing_registry_loads_empty() {
        let fs = MemFs::default();
        assert!(FsSessionRegistry::new("/sessions", fs)
            .load()
            .unwrap()
            .is_empty());
    }

    #[test]
    fn with_persistence_rehydrates_and_continues_ids() {
        let fs = MemFs::default();
        {
            let store =
                SessionStore::with_persistence(Arc::new(FsSessionRegistry::new("/s", fs.clone())));
            assert_eq!(store.create().id, SessionId(1));
            assert_eq!(store.create().id, SessionId(2));
        }
        // A fresh store over the same backing restores the set and mints the next
        // id past the highest restored one — never reusing a persisted id.
        let store = SessionStore::with_persistence(Arc::new(FsSessionRegistry::new("/s", fs)));
        assert_eq!(store.count(), 2);
        assert!(store.contains(SessionId(1)));
        assert!(store.contains(SessionId(2)));
        assert_eq!(store.create().id, SessionId(3));
    }

    #[test]
    fn delete_is_persisted() {
        let fs = MemFs::default();
        {
            let store =
                SessionStore::with_persistence(Arc::new(FsSessionRegistry::new("/s", fs.clone())));
            let first = store.create();
            store.create();
            store.delete(first.id).unwrap();
        }
        let store = SessionStore::with_persistence(Arc::new(FsSessionRegistry::new("/s", fs)));
        assert_eq!(store.count(), 1);
    }

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
