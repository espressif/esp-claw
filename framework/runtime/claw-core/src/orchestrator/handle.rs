use std::collections::HashSet;
use std::sync::mpsc;
use std::sync::{Arc, Mutex, RwLock};

use async_channel::Sender;
use claw_api::{ClawApiConfig, InitError};
use claw_checkpoint::{
    CheckpointCoordinatorInitError, CheckpointStorage, FsCheckpointStorage,
    SharedCheckpointCoordinator,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{
    ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, CoreAffinity, Priority, WorkerHandle,
};
use claw_tool::ToolRegistry;

use crate::config::{ApiUsage, ClawApiManager};
use crate::event::EventSink;
use crate::session::{SessionId, SessionPersistence, SessionStore};

use super::checkpoint::{
    checkpoint_session_registry, load_session_store_state, SessionRegistryCheckpointError,
};
use super::engine::{run_engine, Command};
use super::{
    OpenSessionError, OrchestratorBuildError, SessionControl, SessionControlError,
    SessionEventStream, CHECKPOINT_DIR, CHECKPOINT_HISTORY, CHECKPOINT_INTERVAL,
    ENGINE_WORKER_STACK_SIZE,
};

type CheckpointSessions = dyn Fn(&SessionStore, Option<SessionId>) -> Result<(), SessionRegistryCheckpointError>
    + Send
    + Sync;

/// A `Send + Sync` handle to a running orchestrator.
///
/// Cloning is intentionally not provided: the handle owns the worker's lifetime
/// and joins it on drop. Wrap it in an `Arc` to share.
pub struct Orchestrator {
    sessions: Arc<SessionStore>,
    command_tx: Sender<Command>,
    worker: Mutex<Option<WorkerHandle>>,
    pending_runtime_removals: Arc<Mutex<HashSet<SessionId>>>,
    checkpoint_sessions: Box<CheckpointSessions>,
    /// Shared with the engine worker: turns read the per-usage config from it at
    /// their start; this handle side updates it via [`link_api`](Self::link_api).
    api_manager: Arc<RwLock<ClawApiManager>>,
}

impl Orchestrator {
    /// Build an orchestrator: spawn the drive worker, construct the engine
    /// inside it, and wait for it to report readiness.
    ///
    /// # Errors
    ///
    /// Returns [`OrchestratorBuildError`] when the worker cannot be spawned or the
    /// engine cannot be assembled inside it.
    pub fn new<Filesystem, Http, Timer, Thread, Executor>(
        tools: Arc<ToolRegistry>,
        persistence_dir: String,
        skill_roots: Vec<String>,
    ) -> Result<Self, OrchestratorBuildError>
    where
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + StreamingHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
        Thread: ClawThread,
        Executor: ClawExecutor + 'static,
    {
        let persistence_root = persistence_dir.trim_end_matches('/');
        let checkpoint_dir = format!("{persistence_root}/{CHECKPOINT_DIR}");
        let checkpoints = SharedCheckpointCoordinator::new(
            FsCheckpointStorage::<Filesystem>::new(checkpoint_dir),
            CHECKPOINT_INTERVAL,
            CHECKPOINT_HISTORY,
        )
        .map_err(|error| match error {
            CheckpointCoordinatorInitError::Storage(source) => {
                OrchestratorBuildError::CheckpointStorage(source)
            }
            CheckpointCoordinatorInitError::InvalidCheckpointInterval
            | CheckpointCoordinatorInitError::InvalidHistoryCheckpoints => {
                unreachable!("fixed checkpoint coordinator policy is valid")
            }
        })?;
        Self::new_with_checkpoint_coordinator::<Filesystem, Http, Timer, Thread, Executor>(
            tools,
            checkpoints,
            persistence_dir,
            skill_roots,
        )
    }

    #[doc(hidden)]
    pub fn new_with_checkpoint_coordinator<Filesystem, Http, Timer, Thread, Executor>(
        tools: Arc<ToolRegistry>,
        checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
        persistence_dir: String,
        skill_roots: Vec<String>,
    ) -> Result<Self, OrchestratorBuildError>
    where
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + StreamingHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
        Thread: ClawThread,
        Executor: ClawExecutor + 'static,
    {
        let persistence_root = persistence_dir.trim_end_matches('/');
        let checkpoint_dir = format!("{persistence_root}/{CHECKPOINT_DIR}");
        let session_state = load_session_store_state::<Filesystem>(&checkpoint_dir)?;
        let sessions = Arc::new(SessionStore::new(session_state));
        let (command_tx, command_rx) = async_channel::unbounded();
        let (ready_tx, ready_rx) = mpsc::channel();
        let checkpoint_sessions_coordinator = checkpoints.clone();
        let pending_runtime_removals = Arc::new(Mutex::new(HashSet::new()));
        let checkpoint_pending_removals = Arc::clone(&pending_runtime_removals);
        let checkpoint_sessions = Box::new(
            move |sessions: &SessionStore, removed_session: Option<SessionId>| {
                let mut pending = checkpoint_pending_removals
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
                if let Some(session) = removed_session {
                    pending.insert(session);
                }
                let removed_sessions = pending.iter().copied().collect::<Vec<_>>();
                let result = checkpoint_session_registry::<Filesystem>(
                    &checkpoint_sessions_coordinator,
                    sessions,
                    &removed_sessions,
                );
                if result.is_ok() {
                    pending.clear();
                }
                result
            },
        );

        let api_manager = Arc::new(RwLock::new(ClawApiManager::new()));
        let api_manager_engine = Arc::clone(&api_manager);

        let sessions_engine = Arc::clone(&sessions);
        let worker = Thread::spawn_worker(
            "claw_orchestrator",
            ENGINE_WORKER_STACK_SIZE,
            Priority::Normal,
            CoreAffinity::Any,
            move || {
                run_engine::<Filesystem, Http, Timer, Executor>(
                    tools,
                    checkpoints,
                    persistence_dir,
                    checkpoint_dir,
                    skill_roots,
                    sessions_engine,
                    command_rx,
                    ready_tx,
                    api_manager_engine,
                );
            },
        )?;

        match ready_rx.recv() {
            Ok(Ok(())) => Ok(Self {
                sessions,
                command_tx,
                worker: Mutex::new(Some(worker)),
                pending_runtime_removals,
                checkpoint_sessions,
                api_manager,
            }),
            Ok(Err(error)) => {
                worker.join();
                Err(error)
            }
            Err(_) => {
                worker.join();
                Err(OrchestratorBuildError::WorkerExitedBeforeReady)
            }
        }
    }

    /// Register an LLM API config for a usage (see `ClawApiManager::link_api`).
    ///
    /// Takes `&self`: the manager is behind an `RwLock`, and updates are picked up
    /// at the start of the next turn (turns snapshot their config at their start),
    /// so this never interrupts an in-flight turn.
    ///
    /// # Errors
    ///
    /// Returns [`InitError`] without changing bindings when `api` is invalid.
    pub fn link_api(
        &self,
        api: ClawApiConfig,
        usage: ApiUsage,
        default: bool,
    ) -> Result<(), InitError> {
        self.api_manager
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .link_api(api, usage, default)
    }

    /// Open the session's long-lived event stream and return its write/control
    /// half plus its read half.
    ///
    /// # Errors
    ///
    /// Returns [`OpenSessionError::SessionNotFound`] when `session_id` is not
    /// live, [`OpenSessionError::AlreadyOpen`] when the session already has an
    /// event stream, or [`OpenSessionError::WorkerStopped`] if the engine worker
    /// is gone.
    pub fn open_session(
        &self,
        session_id: SessionId,
    ) -> Result<(SessionControl, SessionEventStream), OpenSessionError> {
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(name: "open_rejected", reason = "session_not_found");
            return Err(OpenSessionError::SessionNotFound(session_id));
        }
        let (sender, receiver) = async_channel::unbounded();
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        let events = EventSink::new(sender);
        self.command_tx
            .try_send(Command::OpenSession {
                session: session_id,
                events,
                ack: ack_tx,
            })
            .map_err(|_| {
                tracing::error!(name: "open_rejected", reason = "worker_stopped");
                OpenSessionError::WorkerStopped
            })?;
        match ack_rx.recv_blocking() {
            Ok(Ok(())) => {
                tracing::info!(name: "opened", "");
                Ok((
                    SessionControl::new(session_id, self.command_tx.clone()),
                    SessionEventStream::new(receiver),
                ))
            }
            Ok(Err(error)) => {
                match &error {
                    OpenSessionError::SessionNotFound(_) => {
                        tracing::warn!(name: "open_rejected", reason = "session_not_found");
                    }
                    OpenSessionError::AlreadyOpen(_) => {
                        tracing::warn!(name: "open_rejected", reason = "already_open");
                    }
                    OpenSessionError::WorkerStopped => {
                        tracing::error!(name: "open_rejected", reason = "worker_stopped");
                    }
                }
                Err(error)
            }
            Err(_) => {
                tracing::error!(name: "open_rejected", reason = "worker_stopped");
                Err(OpenSessionError::WorkerStopped)
            }
        }
    }

    /// Create a fresh isolated conversation session.
    pub fn session_create(&self, persistence: SessionPersistence) -> SessionId {
        let span = tracing::info_span!("session.create");
        let _enter = span.enter();
        let session = self.sessions.create(persistence);
        if persistence == SessionPersistence::Persistent {
            if let Err(error) = (self.checkpoint_sessions)(&self.sessions, None) {
                tracing::error!(name: "checkpoint_failed", target = "session_registry", error = %error);
            }
        }
        tracing::info!(name: "created", session = %session, persistence = ?persistence);
        session
    }

    /// The live conversation sessions, sorted by id.
    pub fn session_list(&self) -> Vec<SessionId> {
        self.sessions.list()
    }

    /// Delete a live session id and remove any associated runtime state.
    ///
    /// If the session has an open event stream, the stream receives
    /// [`crate::event::SessionEvent::Closed`] before it terminates.
    ///
    /// # Errors
    ///
    /// Returns [`SessionControlError::SessionClosed`] when the session id is not
    /// live, or [`SessionControlError::WorkerStopped`] if the engine worker is
    /// gone.
    pub fn session_delete(&self, session_id: SessionId) -> Result<(), SessionControlError> {
        let span = tracing::info_span!("session.delete", run.session = %session_id);
        let _enter = span.enter();
        let Some(persistence) = self.sessions.persistence(session_id) else {
            tracing::warn!(name: "delete_rejected", reason = "session_closed");
            return Err(SessionControlError::SessionClosed(session_id));
        };
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .try_send(Command::DeleteSession {
                session: session_id,
                ack: ack_tx,
            })
            .map_err(|_| {
                tracing::error!(name: "delete_rejected", reason = "worker_stopped");
                SessionControlError::WorkerStopped
            })?;
        match ack_rx.recv_blocking() {
            Ok(Ok(())) => {
                if persistence == SessionPersistence::Persistent {
                    if let Err(error) = (self.checkpoint_sessions)(&self.sessions, Some(session_id))
                    {
                        tracing::error!(
                            name: "checkpoint_failed",
                            target = "session_registry",
                            error = %error
                        );
                    }
                }
                Ok(())
            }
            Ok(Err(error)) => {
                match &error {
                    SessionControlError::SessionClosed(_) => {
                        tracing::warn!(name: "delete_rejected", reason = "session_closed");
                    }
                    SessionControlError::Busy(_) => {
                        tracing::warn!(name: "delete_rejected", reason = "busy");
                    }
                    SessionControlError::WorkerStopped => {
                        tracing::error!(name: "delete_rejected", reason = "worker_stopped");
                    }
                    SessionControlError::ClosePersistence => {
                        tracing::error!(name: "delete_rejected", reason = "persistence");
                    }
                }
                Err(error)
            }
            Err(_) => {
                tracing::error!(name: "delete_rejected", reason = "worker_stopped");
                Err(SessionControlError::WorkerStopped)
            }
        }
    }
}

impl Drop for Orchestrator {
    fn drop(&mut self) {
        let has_pending_removals = !self
            .pending_runtime_removals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .is_empty();
        if has_pending_removals {
            if let Err(error) = (self.checkpoint_sessions)(&self.sessions, None) {
                tracing::error!(
                    name: "checkpoint_failed",
                    target = "session_registry_shutdown",
                    error = %error
                );
            }
        }
        let _ = self.command_tx.try_send(Command::Stop);
        if let Some(worker) = self
            .worker
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .take()
        {
            worker.join();
        }
    }
}
