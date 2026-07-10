use std::sync::mpsc;
use std::sync::{Arc, Mutex};

use async_channel::Sender;
use claw_api::ClawApiConfig;
use claw_interface::{
    ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, CoreAffinity, Priority, WorkerHandle,
};
use claw_tool::ToolRegistry;

use crate::event::EventSink;
use crate::session::{SessionId, SessionStore};

use super::checkpoint::{
    checkpoint_session_registry, load_session_store_state, SessionRegistryCheckpointError,
};
use super::engine::{run_engine, Command};
use super::{
    OpenSessionError, OrchestratorBuildError, SessionControl, SessionControlError,
    SessionEventStream, CHECKPOINT_DIR, ENGINE_WORKER_STACK_SIZE,
};

/// A `Send + Sync` handle to a running orchestrator.
///
/// Cloning is intentionally not provided: the handle owns the worker's lifetime
/// and joins it on drop. Wrap it in an `Arc` to share.
pub struct Orchestrator {
    sessions: Arc<SessionStore>,
    command_tx: Sender<Command>,
    worker: Mutex<Option<WorkerHandle>>,
    checkpoint_sessions:
        Box<dyn Fn(&SessionStore) -> Result<(), SessionRegistryCheckpointError> + Send + Sync>,
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
        llm_config: ClawApiConfig,
        persistence_dir: String,
        skill_roots: Vec<String>,
    ) -> Result<Self, OrchestratorBuildError>
    where
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + Default + 'static,
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
        let checkpoint_sessions_dir = checkpoint_dir.clone();
        let checkpoint_sessions = Box::new(move |sessions: &SessionStore| {
            checkpoint_session_registry::<Filesystem>(&checkpoint_sessions_dir, sessions)
        });

        let sessions_engine = Arc::clone(&sessions);
        let worker = Thread::spawn_worker(
            "claw_orchestrator",
            ENGINE_WORKER_STACK_SIZE,
            Priority::Normal,
            CoreAffinity::Any,
            move || {
                run_engine::<Filesystem, Http, Timer, Executor>(
                    tools,
                    llm_config,
                    persistence_dir,
                    checkpoint_dir,
                    skill_roots,
                    sessions_engine,
                    command_rx,
                    ready_tx,
                );
            },
        )?;

        match ready_rx.recv() {
            Ok(Ok(())) => Ok(Self {
                sessions,
                command_tx,
                worker: Mutex::new(Some(worker)),
                checkpoint_sessions,
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
    pub fn session_create(&self) -> SessionId {
        let span = tracing::info_span!("session.create");
        let _enter = span.enter();
        let session = self.sessions.create();
        if let Err(error) = (self.checkpoint_sessions)(&self.sessions) {
            tracing::error!(name: "checkpoint_failed", target = "session_registry", error = %error);
        }
        tracing::info!(name: "created", session = %session);
        session
    }

    /// The live conversation sessions, sorted by id.
    pub fn session_list(&self) -> Vec<SessionId> {
        let mut sessions = self.sessions.list();
        sessions.sort_by_key(|id| id.0);
        sessions
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
        if !self.sessions.contains(session_id) {
            tracing::warn!(name: "delete_rejected", reason = "session_closed");
            return Err(SessionControlError::SessionClosed(session_id));
        }
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
                if let Err(error) = (self.checkpoint_sessions)(&self.sessions) {
                    tracing::error!(
                        name: "checkpoint_failed",
                        target = "session_registry",
                        error = %error
                    );
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
