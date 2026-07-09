//! Layer 1 orchestrator: a `Send` handle over a `!Send` drive engine that runs
//! on one owned worker thread.
//!
//! [`Orchestrator`] is a cheap, `Send + Sync` handle: it validates sessions
//! synchronously (via a shared [`SessionStore`]) and forwards work to the engine
//! as [`Command`]s over an async channel. The engine ([`Engine`]) is `!Send` (it
//! owns `Box<dyn Agent>` graphs), so it is built *inside* the worker thread from
//! `Send` config and `block_on`-driven there. The engine multiplexes every live
//! session's drive on that single thread — cooperative concurrency, not
//! parallelism — the same way the async HTTP seam yields between EAGAIN steps.
//!
//! Channel routing is owned by the layer above this crate.

mod approval;
mod control;
mod instance;

use core::cell::RefCell;
use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::borrow::Cow;
use std::collections::{HashMap, VecDeque};
use std::io;
use std::rc::Rc;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};

use async_channel::{Receiver, Sender};
use claw_api::{ClawApiConfig, InitError};
use claw_checkpoint::{
    BatchId, BatchWrite, ChangePatternHint, CheckpointStorage, CheckpointStorageError,
    CheckpointWrite, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    FsCheckpointStorage, LoadCheckpointError, PartGeneration, PartStateBlob, PartStateSlice,
    PartWrite, StorageHint, StorageSizeHint,
};
use claw_interface::{
    ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, CoreAffinity, Priority, WorkerHandle,
};
use claw_memory::LongTermInitError;
use claw_skill::SkillError;
use claw_tool::ToolRegistry;
use futures_core::Stream;
use serde::{Deserialize, Serialize};
use strum::IntoStaticStr;
use tracing::Instrument as _;

use crate::agent::{AgentIdAllocator, CancelReason, FsAgentFactory, FsAgentFactoryError};
use crate::event::{EventSink, SessionEvent, TurnCause};
use crate::session::{SessionId, SessionStore, SessionStoreState, TurnId, TurnIdAllocator};

pub use self::instance::{DriveOutput, RootReply};

use self::approval::{ApprovalResolverError, PermissionReplyResolution};
use self::control::{DriveControl, DriveStop};
use self::instance::{
    ApprovalResolutionError, InstanceDeliverError, OrchestratorInstance,
    OrchestratorInstanceRestoreError, OrchestratorInstanceState, PendingApproval,
};

/// Stack for the orchestrator's single drive worker. It runs the whole agent
/// graph (context building, LLM round-trips, tool calls), so it matches the
/// device agent worker's budget.
const ENGINE_WORKER_STACK_SIZE: usize = 64 * 1024;
const CHECKPOINT_DIR: &str = "checkpoint";
const ENGINE_BATCH: &str = "engine";
const ENGINE_BATCH_ID: BatchId = BatchId::new(1);
const ENGINE_PART: &str = "engine";
const SESSION_REGISTRY_BATCH: &str = "session-registry";
const SESSION_REGISTRY_BATCH_ID: BatchId = BatchId::new(1);
const SESSION_STORE_PART: &str = "session-store";
const SESSION_RUNTIME_BATCH: &str = "session-runtime";
const SESSION_DRIVE_PART: &str = "session-drive";
const ORCHESTRATOR_INSTANCE_PART: &str = "orchestrator-instance";

/// A per-session drive future owned by the engine's run loop while it is in
/// flight. `!Send`/`!'static`-free: it borrows nothing outside the `Rc<Engine>`
/// it captures.
type DriveFuture = Pin<Box<dyn Future<Output = ()>>>;

#[derive(Clone, Copy, Debug, IntoStaticStr, PartialEq, Eq)]
enum ControlOp {
    #[strum(serialize = "interrupt")]
    Interrupt,
    #[strum(serialize = "cancel")]
    Cancel,
}

impl ControlOp {
    fn merge(existing: Option<Self>, incoming: Self) -> Self {
        match (existing, incoming) {
            (Some(Self::Cancel), _) | (_, Self::Cancel) => Self::Cancel,
            _ => Self::Interrupt,
        }
    }
}

/// One accepted user input waiting to be handed to the session pump.
#[derive(Deserialize, Serialize)]
struct SubmittedInput {
    text: String,
}

#[derive(Deserialize, Serialize)]
struct SessionDriveState {
    pending_input: Option<SubmittedInput>,
    next_turn_id: TurnId,
}

impl Default for SessionDriveState {
    fn default() -> Self {
        Self {
            pending_input: None,
            next_turn_id: TurnIdAllocator::new().peek(),
        }
    }
}

impl DurableStateCodec for SessionDriveState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }
}

struct SessionDrive {
    events: Option<EventSink>,
    running: bool,
    foreground_active: bool,
    control: Option<DriveControl>,
    requested_control: Option<ControlOp>,
    closing: bool,
    close_cancels: bool,
    state: DurableState<SessionDriveState>,
}

impl SessionDrive {
    fn new(state: SessionDriveState) -> Self {
        Self {
            events: None,
            running: false,
            foreground_active: false,
            control: None,
            requested_control: None,
            closing: false,
            close_cancels: false,
            state: DurableState::new(state),
        }
    }

    fn has_pending_input(&self) -> bool {
        self.state.get().pending_input.is_some()
    }

    fn set_pending_input(&mut self, input: SubmittedInput) {
        self.state.get_mut().pending_input = Some(input);
    }

    fn take_pending_input(&mut self) -> Option<SubmittedInput> {
        if self.state.get().pending_input.is_none() {
            return None;
        }
        self.state.get_mut().pending_input.take()
    }

    fn next_turn(&mut self) -> TurnId {
        let state = self.state.get_mut();
        let turn = state.next_turn_id;
        state.next_turn_id = TurnId::new(turn.0.saturating_add(1));
        turn
    }
}

impl DurablePart for SessionDrive {
    fn name(&self) -> &'static str {
        "session-drive"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(Self::new(SessionDriveState::decode_state(state)?))
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}

/// A command the [`Orchestrator`] handle sends to its engine worker.
///
/// Session create/list live entirely on the handle (`SessionStore` is shared and
/// synchronous), so they need no command. `CloseSession` is a command because
/// the engine must detach the live event stream and cancel active work.
/// `DeleteSession` removes the registry entry and session runtime state. `Stop`
/// lets shutdown drain in-flight drives and exit the run loop so the worker
/// joins.
enum Command {
    OpenSession {
        session: SessionId,
        events: EventSink,
        ack: Sender<Result<(), OpenSessionError>>,
    },
    Submit {
        session: SessionId,
        text: String,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Control {
        session: SessionId,
        op: ControlOp,
        ack: Sender<Result<(), SessionControlError>>,
    },
    CloseSession {
        session: SessionId,
        ack: Sender<Result<(), SessionControlError>>,
    },
    DeleteSession {
        session: SessionId,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Stop,
}

/// Failure opening a session event stream.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum OpenSessionError {
    /// The requested session id is not live.
    #[error("session not found: {0}")]
    SessionNotFound(SessionId),
    /// This session already has an open event stream.
    #[error("session is already open: {0}")]
    AlreadyOpen(SessionId),
    /// The orchestrator worker is gone, so the open request could not be
    /// delivered.
    #[error("orchestrator worker is not running")]
    WorkerStopped,
}

/// Failure sending a command through a session control handle.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum SessionControlError {
    /// The session is missing, closed, or was never opened for events.
    #[error("session is closed: {0}")]
    SessionClosed(SessionId),
    /// The session already has a foreground submit being driven or ready to run.
    #[error("session is busy: {0}")]
    Busy(SessionId),
    /// The orchestrator worker is gone, so the command could not be delivered.
    #[error("orchestrator worker is not running")]
    WorkerStopped,
}

/// Cloneable write/control half of an open session.
#[derive(Clone)]
pub struct SessionControl {
    session: SessionId,
    command_tx: Sender<Command>,
}

impl SessionControl {
    fn new(session: SessionId, command_tx: Sender<Command>) -> Self {
        Self {
            session,
            command_tx,
        }
    }

    /// Submit one user input for this session.
    ///
    /// The returned future resolves when the orchestrator accepts the command,
    /// not when the agent reply completes. If a foreground submit is already in
    /// progress, this returns [`SessionControlError::Busy`] instead of buffering
    /// another input internally. User-visible output is delivered on the paired
    /// [`SessionEventStream`].
    pub async fn submit(&self, text: impl Into<String>) -> Result<(), SessionControlError> {
        let text = text.into();
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::Submit {
                session: self.session,
                text,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }

    /// Gracefully stop the current foreground drive at the next safe boundary.
    pub async fn interrupt(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Interrupt).await
    }

    /// Hard-cancel foreground and background work in this session.
    pub async fn cancel(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Cancel).await
    }

    /// Close this open session stream. The session id remains live and can be
    /// opened again later; deleting the session is handled by
    /// [`Orchestrator::session_delete`].
    pub async fn close_session(&self) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::CloseSession {
                session: self.session,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }

    async fn send_control(&self, op: ControlOp) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::Control {
                session: self.session,
                op,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }
}

/// The read/event half of an open session.
pub struct SessionEventStream {
    events: Pin<Box<Receiver<SessionEvent>>>,
}

impl SessionEventStream {
    fn new(events: Receiver<SessionEvent>) -> Self {
        Self {
            events: Box::pin(events),
        }
    }
}

impl Stream for SessionEventStream {
    type Item = SessionEvent;

    fn poll_next(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Option<SessionEvent>> {
        self.get_mut().events.as_mut().poll_next(context)
    }
}

fn apply_control(control: &DriveControl, op: Option<ControlOp>) {
    match op {
        Some(ControlOp::Interrupt) => control.request_interrupt(),
        Some(ControlOp::Cancel) => control.request_cancel(),
        None => {}
    }
}

/// What can go wrong while building an [`Orchestrator`].
#[derive(Debug, thiserror::Error)]
pub enum OrchestratorBuildError {
    /// No persistence directory was provided.
    #[error("persistence directory is required")]
    MissingPersistenceDir,
    /// The dedicated extraction LLM client (for long-term memory) failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(#[from] InitError),
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(#[from] LongTermInitError),
    /// The configured skill catalog could not be scanned.
    #[error("failed to load skill catalog: {0}")]
    SkillRegistry(#[from] SkillError),
    /// Checkpoint storage metadata could not be read while booting.
    #[error("failed to read checkpoint storage: {0}")]
    CheckpointStorage(#[from] CheckpointStorageError),
    /// The latest checkpoint exists but cannot be loaded.
    #[error("failed to load checkpoint: {0}")]
    CheckpointLoad(#[from] LoadCheckpointError),
    /// A checkpoint part exists but cannot be decoded into runtime state.
    #[error("failed to restore checkpoint part: {0}")]
    CheckpointRestore(#[from] DurablePartError),
    /// A session instance checkpoint exists but cannot be restored.
    #[error("failed to restore checkpointed session instance: {0}")]
    CheckpointInstanceRestore(#[from] OrchestratorInstanceRestoreError),
    /// A checkpoint batch exists but does not contain the expected durable part.
    #[error("checkpoint is missing part {part} in batch {batch}")]
    MissingCheckpointPart {
        batch: &'static str,
        part: &'static str,
    },
    /// The drive worker thread could not be spawned.
    #[error("failed to spawn the orchestrator worker: {0}")]
    WorkerSpawn(#[from] io::Error),
    /// The drive worker exited before reporting startup success or failure.
    #[error("orchestrator worker exited before signalling readiness")]
    WorkerExitedBeforeReady,
}

impl From<FsAgentFactoryError> for OrchestratorBuildError {
    fn from(error: FsAgentFactoryError) -> Self {
        match error {
            FsAgentFactoryError::MissingPersistenceDir => Self::MissingPersistenceDir,
            FsAgentFactoryError::ExtractionLlm(source) => Self::ExtractionLlm(source),
            FsAgentFactoryError::LongTermInit(source) => Self::LongTermInit(source),
            FsAgentFactoryError::SkillRegistry(source) => Self::SkillRegistry(source),
        }
    }
}

#[derive(Debug, thiserror::Error)]
enum SessionRegistryCheckpointError {
    #[error(transparent)]
    Storage(#[from] CheckpointStorageError),
    #[error(transparent)]
    Export(#[from] DurablePartError),
}

#[derive(Debug, thiserror::Error)]
enum RuntimeCheckpointError {
    #[error(transparent)]
    Storage(#[from] CheckpointStorageError),
    #[error(transparent)]
    Export(#[from] DurablePartError),
}

fn load_session_store_state<Filesystem: ClawFs>(
    checkpoint_dir: &str,
) -> Result<SessionStoreState, OrchestratorBuildError> {
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_dir.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(SessionStoreState::default());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut saw_batch = false;
    for batch in checkpoint.batches {
        if batch.name != SESSION_REGISTRY_BATCH || batch.id != SESSION_REGISTRY_BATCH_ID {
            continue;
        }
        saw_batch = true;
        for part in batch.parts {
            if part.name == SESSION_STORE_PART {
                return Ok(SessionStoreState::decode_state(part.state.as_slice())?);
            }
        }
    }
    if saw_batch {
        Err(OrchestratorBuildError::MissingCheckpointPart {
            batch: SESSION_REGISTRY_BATCH,
            part: SESSION_STORE_PART,
        })
    } else {
        Ok(SessionStoreState::default())
    }
}

fn load_engine_state<Filesystem: ClawFs>(
    checkpoint_dir: &str,
) -> Result<EngineState, OrchestratorBuildError> {
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_dir.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(EngineState::default());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut saw_batch = false;
    for batch in checkpoint.batches {
        if batch.name != ENGINE_BATCH || batch.id != ENGINE_BATCH_ID {
            continue;
        }
        saw_batch = true;
        for part in batch.parts {
            if part.name == ENGINE_PART {
                return Ok(EngineState::decode_state(part.state.as_slice())?);
            }
        }
    }
    if saw_batch {
        Err(OrchestratorBuildError::MissingCheckpointPart {
            batch: ENGINE_BATCH,
            part: ENGINE_PART,
        })
    } else {
        Ok(EngineState::default())
    }
}

fn load_session_drives<Filesystem: ClawFs>(
    checkpoint_dir: &str,
    sessions: &SessionStore,
) -> Result<HashMap<SessionId, SessionDrive>, OrchestratorBuildError> {
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_dir.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(HashMap::new());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut drives = HashMap::new();
    for batch in checkpoint.batches {
        if batch.name != SESSION_RUNTIME_BATCH {
            continue;
        }
        let session = SessionId::new(batch.id.0);
        if !sessions.contains(session) {
            continue;
        }
        let mut saw_drive = false;
        for part in batch.parts {
            if part.name == SESSION_DRIVE_PART {
                saw_drive = true;
                let state = SessionDriveState::decode_state(part.state.as_slice())?;
                drives.insert(session, SessionDrive::new(state));
            }
        }
        if !saw_drive {
            return Err(OrchestratorBuildError::MissingCheckpointPart {
                batch: SESSION_RUNTIME_BATCH,
                part: SESSION_DRIVE_PART,
            });
        }
    }
    Ok(drives)
}

fn load_session_instances<Filesystem, Http, Timer>(
    checkpoint_dir: &str,
    sessions: &SessionStore,
    factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
    agent_id_allocator: AgentIdAllocator,
) -> Result<HashMap<SessionId, OrchestratorInstance<Filesystem, Http, Timer>>, OrchestratorBuildError>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_dir.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(HashMap::new());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut instances = HashMap::new();
    for batch in checkpoint.batches {
        if batch.name != SESSION_RUNTIME_BATCH {
            continue;
        }
        let session = SessionId::new(batch.id.0);
        if !sessions.contains(session) {
            continue;
        }
        for part in batch.parts {
            if part.name == ORCHESTRATOR_INSTANCE_PART {
                let state = OrchestratorInstanceState::decode_state(part.state.as_slice())?;
                let instance = OrchestratorInstance::from_restored_state(
                    session,
                    Arc::clone(&factory),
                    agent_id_allocator.clone(),
                    state,
                )?;
                instances.insert(session, instance);
            }
        }
    }
    Ok(instances)
}

#[derive(Debug, IntoStaticStr, thiserror::Error)]
enum DeliverError {
    #[strum(serialize = "agent")]
    #[error(transparent)]
    Instance(#[from] InstanceDeliverError),
    #[strum(serialize = "agent")]
    #[error(transparent)]
    ApprovalResolver(#[from] ApprovalResolverError),
    #[strum(serialize = "agent")]
    #[error(transparent)]
    ApprovalResolution(#[from] ApprovalResolutionError),
}

/// A `Send + Sync` handle to a running orchestrator.
///
/// Cloning is intentionally not provided: the handle owns the worker's lifetime
/// and joins it on drop. Wrap it in an `Arc` to share.
pub struct Orchestrator {
    /// The session id truth source, shared with the engine so the handle can
    /// validate opens/deletes before forwarding work.
    sessions: Arc<SessionStore>,
    /// Outbound command channel to the engine worker.
    command_tx: Sender<Command>,
    /// The drive worker, joined on drop. `Mutex<Option<..>>` so `Drop` can take
    /// it out behind a shared borrow.
    worker: Mutex<Option<WorkerHandle>>,
    checkpoint_sessions:
        Box<dyn Fn(&SessionStore) -> Result<(), SessionRegistryCheckpointError> + Send + Sync>,
}

impl Orchestrator {
    /// Build an orchestrator: spawn the drive worker (via
    /// `Thread::spawn_worker`), construct the engine inside it, and wait for it
    /// to report readiness.
    ///
    /// `llm_config` is cloned into every agent, `persistence_dir` is the storage
    /// root the engine's factory owns, and `skill_roots` are the priority-ordered
    /// skill directories every agent's catalog is built from. Owned config is
    /// moved into the worker closure directly so startup does not duplicate it.
    ///
    /// # Errors
    ///
    /// Returns [`OrchestratorBuildError`] when the worker cannot be spawned or the
    /// engine (factory) cannot be assembled inside it.
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
        let checkpoint_dir = if persistence_dir == "/" {
            format!("/{CHECKPOINT_DIR}")
        } else if persistence_root.is_empty() {
            CHECKPOINT_DIR.to_owned()
        } else {
            format!("{persistence_root}/{CHECKPOINT_DIR}")
        };
        let session_state = load_session_store_state::<Filesystem>(&checkpoint_dir)?;
        let sessions = Arc::new(SessionStore::new(session_state));
        let (command_tx, command_rx) = async_channel::unbounded();
        let (ready_tx, ready_rx) = mpsc::channel();
        let checkpoint_sessions_dir = checkpoint_dir.clone();
        let checkpoint_sessions = Box::new(move |sessions: &SessionStore| {
            let mut storage =
                FsCheckpointStorage::<Filesystem>::new(checkpoint_sessions_dir.clone());
            let step = storage.next_step()?;
            let state = sessions.export_state()?;
            let hint = sessions.storage_hint();
            storage.write_checkpoint(CheckpointWrite {
                step,
                batches: vec![BatchWrite {
                    batch: (SESSION_REGISTRY_BATCH, SESSION_REGISTRY_BATCH_ID),
                    writes: vec![PartWrite {
                        name: SESSION_STORE_PART,
                        state,
                        hint,
                    }],
                }],
            })?;
            Ok(())
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
    /// [`SessionEvent::Closed`] before it terminates.
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
        // Ask the engine to drain in-flight drives and exit, then join the worker.
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

/// Worker entry point: build the `!Send` engine from `Send` config, signal
/// readiness, then `block_on` its run loop until the command channel closes or a
/// `Stop` drains it.
fn run_engine<Filesystem, Http, Timer, Executor>(
    tools: Arc<ToolRegistry>,
    llm_config: ClawApiConfig,
    persistence_dir: String,
    checkpoint_dir: String,
    skill_roots: Vec<String>,
    sessions: Arc<SessionStore>,
    command_rx: Receiver<Command>,
    ready: mpsc::Sender<Result<(), OrchestratorBuildError>>,
) where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
    Executor: ClawExecutor,
{
    let engine_state = match load_engine_state::<Filesystem>(&checkpoint_dir) {
        Ok(state) => state,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let engine = match Engine::<Filesystem, Http, Timer>::new(
        tools,
        llm_config,
        persistence_dir,
        checkpoint_dir,
        skill_roots,
        sessions,
        engine_state,
    ) {
        Ok(engine) => Rc::new(engine),
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let _ = ready.send(Ok(()));
    // Drive the `!Send` engine to completion on this worker thread via the
    // injected executor (`edge-executor` on device, tokio on host).
    Executor::block_on(engine.run(command_rx));
}

/// The `!Send` drive engine. Sole owner of the session instances and their drive
/// state; runs on one worker thread and multiplexes every live session.
struct Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Builds agents for every session's registry.
    factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
    /// Config for the one-shot natural-language approval resolver.
    approval_llm_config: ClawApiConfig,
    /// Checkpoint storage root owned by the orchestrator runtime.
    checkpoint_dir: String,
    /// One isolated agent graph per session. Single-owner interior mutability
    /// (`RefCell`): the engine is `!Send` and driven from one thread.
    instances: RefCell<HashMap<SessionId, OrchestratorInstance<Filesystem, Http, Timer>>>,
    /// Per-session connection, accepted input slot, and active drive control state.
    drives: RefCell<HashMap<SessionId, SessionDrive>>,
    /// Session id truth source, shared with the handle.
    sessions: Arc<SessionStore>,
    /// Durable engine state.
    state: DurableState<EngineState>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct EngineState {
    agent_id_allocator: AgentIdAllocator,
}

impl Default for EngineState {
    fn default() -> Self {
        Self {
            agent_id_allocator: AgentIdAllocator::new(),
        }
    }
}

impl DurableStateCodec for EngineState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }
}

impl<Filesystem, Http, Timer> DurablePart for Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn name(&self) -> &'static str {
        "engine"
    }

    fn generation(&self) -> PartGeneration {
        u64::from(self.state.get().agent_id_allocator.peek().0)
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn new(
        tools: Arc<ToolRegistry>,
        llm_config: ClawApiConfig,
        persistence_dir: String,
        checkpoint_dir: String,
        skill_roots: Vec<String>,
        sessions: Arc<SessionStore>,
        state: EngineState,
    ) -> Result<Self, OrchestratorBuildError> {
        let factory = Arc::new(FsAgentFactory::<Filesystem, Http, Timer>::new(
            tools,
            llm_config.clone(),
            persistence_dir,
            skill_roots,
        )?);
        let drives = load_session_drives::<Filesystem>(&checkpoint_dir, sessions.as_ref())?;
        let instances = load_session_instances::<Filesystem, Http, Timer>(
            &checkpoint_dir,
            sessions.as_ref(),
            Arc::clone(&factory),
            state.agent_id_allocator.clone(),
        )?;
        Ok(Self {
            factory,
            approval_llm_config: llm_config,
            checkpoint_dir,
            instances: RefCell::new(instances),
            drives: RefCell::new(drives),
            sessions,
            state: DurableState::new(state),
        })
    }

    fn checkpoint_session_runtime(
        &self,
        session_id: SessionId,
    ) -> Result<(), RuntimeCheckpointError> {
        let engine_state = self.export_state()?;
        let engine_hint = self.storage_hint();
        let drive_write = {
            let drives = self.drives.borrow();
            let Some(drive) = drives.get(&session_id) else {
                return Ok(());
            };
            let state = drive.export_state()?;
            PartWrite {
                name: SESSION_DRIVE_PART,
                state: PartStateBlob {
                    schema_version: state.schema_version,
                    bytes: Cow::Owned(state.bytes.into_owned()),
                },
                hint: drive.storage_hint(),
            }
        };
        let instance_write = {
            let instances = self.instances.borrow();
            if let Some(instance) = instances.get(&session_id) {
                let state = instance.export_state()?;
                Some(PartWrite {
                    name: ORCHESTRATOR_INSTANCE_PART,
                    state: PartStateBlob {
                        schema_version: state.schema_version,
                        bytes: Cow::Owned(state.bytes.into_owned()),
                    },
                    hint: instance.storage_hint(),
                })
            } else {
                None
            }
        };
        let mut session_writes = vec![drive_write];
        if let Some(instance_write) = instance_write {
            session_writes.push(instance_write);
        }

        let mut storage = FsCheckpointStorage::<Filesystem>::new(self.checkpoint_dir.clone());
        let step = storage.next_step()?;
        storage.write_checkpoint(CheckpointWrite {
            step,
            batches: vec![
                BatchWrite {
                    batch: (ENGINE_BATCH, ENGINE_BATCH_ID),
                    writes: vec![PartWrite {
                        name: ENGINE_PART,
                        state: engine_state,
                        hint: engine_hint,
                    }],
                },
                BatchWrite {
                    batch: (SESSION_RUNTIME_BATCH, BatchId::new(session_id.0)),
                    writes: session_writes,
                },
            ],
        })?;
        Ok(())
    }

    /// Multiplex every active session pump plus the command channel on one
    /// thread. Each running session owns at most one future in `inflight`; the
    /// command receiver is polled alongside them, so session control is observed
    /// between cooperative yields of any running drive.
    async fn run(self: &Rc<Self>, command_rx: Receiver<Command>) {
        let mut command_rx = core::pin::pin!(command_rx);
        let mut inflight: VecDeque<DriveFuture> = VecDeque::new();
        let mut rx_open = true;
        let mut stopping = false;

        loop {
            if (stopping || !rx_open) && inflight.is_empty() {
                if stopping {
                    // Any command queued behind `Stop` will never be driven. Ack
                    // it explicitly so callers do not wait on a reply channel that
                    // can no longer be completed.
                    while let Ok(command) = command_rx.try_recv() {
                        match command {
                            Command::OpenSession { ack, .. } => {
                                let _ = ack.try_send(Err(OpenSessionError::WorkerStopped));
                            }
                            Command::Submit { ack, .. }
                            | Command::Control { ack, .. }
                            | Command::CloseSession { ack, .. }
                            | Command::DeleteSession { ack, .. } => {
                                let _ = ack.try_send(Err(SessionControlError::WorkerStopped));
                            }
                            Command::Stop => {}
                        }
                    }
                }
                return;
            }
            // Once stopping, stop accepting new commands; just drain in-flight.
            let recv = (rx_open && !stopping).then_some(command_rx.as_mut());
            match (EnginePoll {
                inflight: &mut inflight,
                recv,
            })
            .await
            {
                EngineEvent::DriveDone => {}
                EngineEvent::Command(Some(Command::OpenSession {
                    session,
                    events,
                    ack,
                })) => {
                    let (result, future) = self.open_session_stream(session, events);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::Submit { session, text, ack })) => {
                    let (result, future) = self.submit_input(session, SubmittedInput { text });
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::Control { session, op, ack })) => {
                    let (result, future) = self.control_session(session, op);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::CloseSession { session, ack })) => {
                    let (result, future) = self.close_session(session);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::DeleteSession { session, ack })) => {
                    let (result, future) = self.delete_session(session);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::Stop)) => stopping = true,
                EngineEvent::Command(None) => rx_open = false,
            }
        }
    }

    fn open_session_stream(
        self: &Rc<Self>,
        session_id: SessionId,
        events: EventSink,
    ) -> (Result<(), OpenSessionError>, Option<DriveFuture>) {
        if !self.sessions.contains(session_id) {
            return (Err(OpenSessionError::SessionNotFound(session_id)), None);
        }
        let has_pending_input = {
            let mut drives = self.drives.borrow_mut();
            let drive = drives
                .entry(session_id)
                .or_insert_with(|| SessionDrive::new(SessionDriveState::default()));
            if drive.events.is_some() || drive.closing {
                return (Err(OpenSessionError::AlreadyOpen(session_id)), None);
            }
            drive.events = Some(events);
            drive.closing = false;
            drive.close_cancels = false;
            drive.has_pending_input()
        };
        let should_start =
            has_pending_input || !matches!(self.instance_work(session_id), InstanceWork::None);
        let future = should_start
            .then(|| self.ensure_session_drive(session_id))
            .flatten();
        (Ok(()), future)
    }

    fn submit_input(
        self: &Rc<Self>,
        session_id: SessionId,
        input: SubmittedInput,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let has_text = !input.text.is_empty();
        let text_bytes = input.text.len() as u64;
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "submit_rejected",
                reason = "session_closed",
                has_text,
                text_bytes,
                attachment_count = 0_u64,
                attachment_kinds = "none",
            );
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "session_closed",
                    has_text,
                    text_bytes,
                    attachment_count = 0_u64,
                    attachment_kinds = "none",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "session_closed",
                    has_text,
                    text_bytes,
                    attachment_count = 0_u64,
                    attachment_kinds = "none",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if drive.has_pending_input()
                || drive.foreground_active
                || self.instance_has_root_work(session_id)
            {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "busy",
                    has_text,
                    text_bytes,
                    attachment_count = 0_u64,
                    attachment_kinds = "none",
                );
                return (Err(SessionControlError::Busy(session_id)), None);
            }
            drive.set_pending_input(input);
            if let Some(control) = &drive.control {
                control.request_wake();
            }
        }
        tracing::info!(
            name: "submit_accepted",
            has_text,
            text_bytes,
            attachment_count = 0_u64,
            attachment_kinds = "none",
        );
        if let Err(error) = self.checkpoint_session_runtime(session_id) {
            tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
        }
        (Ok(()), self.ensure_session_drive(session_id))
    }

    fn control_session(
        self: &Rc<Self>,
        session_id: SessionId,
        op: ControlOp,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let op_name: &'static str = (&op).into();
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "control_rejected",
                op = op_name,
                reason = "session_closed",
            );
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if op == ControlOp::Cancel {
                let _ = drive.take_pending_input();
            }
            drive.requested_control = Some(ControlOp::merge(drive.requested_control, op));
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
        }
        tracing::info!(name: "control_requested", op = op_name);
        (Ok(()), self.ensure_session_drive(session_id))
    }

    fn close_session(
        self: &Rc<Self>,
        session_id: SessionId,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let session_span = tracing::info_span!("session", run.session = %session_id);
        let _session_enter = session_span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(name: "close_rejected", reason = "session_closed");
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let drives = self.drives.borrow();
            let Some(drive) = drives.get(&session_id) else {
                tracing::warn!(name: "close_rejected", reason = "not_open");
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() {
                tracing::warn!(name: "close_rejected", reason = "not_open");
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if drive.closing {
                tracing::info!(name: "close_requested", "");
                return (Ok(()), None);
            }
            tracing::info!(name: "close_requested", "");
        }
        (Ok(()), self.session_shutdown(session_id))
    }

    fn delete_session(
        self: &Rc<Self>,
        session_id: SessionId,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let session_span = tracing::info_span!("session", run.session = %session_id);
        let _session_enter = session_span.enter();
        let delete_span = tracing::info_span!("session.delete", run.session = %session_id);
        let existed = delete_span.in_scope(|| {
            let existed = self.sessions.delete(session_id);
            if existed {
                tracing::info!(name: "delete_requested", "");
                tracing::info!(name: "registry_removed", "");
            } else {
                tracing::warn!(name: "delete_rejected", reason = "session_closed");
            }
            existed
        });
        if !existed {
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let drives = self.drives.borrow();
            if !drives.contains_key(&session_id) {
                drop(drives);
                self.instances.borrow_mut().remove(&session_id);
                tracing::info_span!("session.delete", run.session = %session_id).in_scope(|| {
                    tracing::info!(name: "runtime_state_removed", "");
                });
                return (Ok(()), None);
            };
        }
        (Ok(()), self.session_shutdown(session_id))
    }

    fn session_shutdown(self: &Rc<Self>, session_id: SessionId) -> Option<DriveFuture> {
        let has_root_work = self.instance_has_root_work(session_id);
        let mut should_start = false;
        {
            let mut drives = self.drives.borrow_mut();
            let drive = drives.get_mut(&session_id)?;
            let had_pending_input = drive.take_pending_input().is_some();
            drive.close_cancels = drive.close_cancels
                || drive.running
                || drive.foreground_active
                || had_pending_input
                || has_root_work;
            drive.closing = true;
            drive.requested_control = Some(ControlOp::Cancel);
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
            if !drive.running {
                drive.running = true;
                should_start = true;
            }
        }
        should_start.then(|| {
            let engine = Rc::clone(self);
            Box::pin(
                async move {
                    engine.drive_session(session_id).await;
                }
                .instrument(tracing::info_span!("session", run.session = %session_id)),
            ) as DriveFuture
        })
    }

    fn ensure_session_drive(self: &Rc<Self>, session_id: SessionId) -> Option<DriveFuture> {
        {
            let mut drives = self.drives.borrow_mut();
            let drive = drives.get_mut(&session_id)?;
            if drive.running {
                if let Some(control) = &drive.control {
                    control.request_wake();
                }
                return None;
            }
            drive.running = true;
        }
        let engine = Rc::clone(self);
        Some(Box::pin(
            async move {
                engine.drive_session(session_id).await;
            }
            .instrument(tracing::info_span!("session", run.session = %session_id)),
        ))
    }

    async fn drive_session(&self, session_id: SessionId) {
        loop {
            if self.session_is_closing(session_id) {
                self.finish_closing_session(session_id).await;
                return;
            }

            if let Some(input) = self.take_input(session_id) {
                self.drive_user_turn(session_id, input).await;
                continue;
            }

            match self.instance_work(session_id) {
                InstanceWork::Root => {
                    self.drive_background_result_turn(session_id).await;
                    continue;
                }
                InstanceWork::Background => {
                    self.drive_background(session_id).await;
                    continue;
                }
                InstanceWork::None => {}
            }

            break;
        }
        self.finish_session_drive(session_id);
    }

    fn session_is_closing(&self, session_id: SessionId) -> bool {
        match self.drives.borrow().get(&session_id) {
            Some(drive) => drive.closing,
            None => true,
        }
    }

    fn take_input(&self, session_id: SessionId) -> Option<SubmittedInput> {
        let mut drives = self.drives.borrow_mut();
        let drive = drives.get_mut(&session_id)?;
        if drive.closing {
            return None;
        }
        drive.take_pending_input()
    }

    fn session_events(&self, session_id: SessionId) -> Option<EventSink> {
        self.drives
            .borrow()
            .get(&session_id)
            .and_then(|drive| drive.events.clone())
    }

    fn finish_session_drive(&self, session_id: SessionId) {
        let mut drives = self.drives.borrow_mut();
        if let Some(drive) = drives.get_mut(&session_id) {
            drive.running = false;
            drive.foreground_active = false;
            drive.control = None;
            drive.requested_control = None;
        }
    }

    fn set_foreground_active(&self, session_id: SessionId, active: bool) {
        if let Some(drive) = self.drives.borrow_mut().get_mut(&session_id) {
            drive.foreground_active = active;
        }
    }

    fn set_active_control(&self, session_id: SessionId, control: Option<DriveControl>) {
        let mut drives = self.drives.borrow_mut();
        if let Some(drive) = drives.get_mut(&session_id) {
            if let Some(control) = &control {
                apply_control(control, drive.requested_control);
            } else {
                drive.requested_control = None;
            }
            drive.control = control;
        }
    }

    async fn drive_user_turn(&self, session_id: SessionId, input: SubmittedInput) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let Some(turn) = self
            .drives
            .borrow_mut()
            .get_mut(&session_id)
            .map(SessionDrive::next_turn)
        else {
            return;
        };
        let has_text = !input.text.is_empty();
        let text_bytes = input.text.len() as u64;
        async {
            self.set_foreground_active(session_id, true);
            events.emit(SessionEvent::TurnStarted {
                turn,
                cause: TurnCause::UserSubmit,
            });
            tracing::info!(
                name: "input_delivered",
                has_text,
                text_bytes,
                attachment_count = 0_u64,
                attachment_kinds = "none",
            );
            let result = self.drive_one_input(session_id, input.text, &events).await;
            self.finish_turn(session_id, turn, &events, result);
            self.set_foreground_active(session_id, false);
        }
        .instrument(tracing::info_span!("turn", run.turn = %turn, cause = "user_submit"))
        .await;
    }

    async fn drive_background_result_turn(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let Some(turn) = self
            .drives
            .borrow_mut()
            .get_mut(&session_id)
            .map(SessionDrive::next_turn)
        else {
            return;
        };
        async {
            self.set_foreground_active(session_id, true);
            events.emit(SessionEvent::TurnStarted {
                turn,
                cause: TurnCause::BackgroundResult,
            });
            tracing::info!(name: "background_result", "");
            let result = self.drive_root_ready(session_id, &events).await;
            self.finish_turn(session_id, turn, &events, result);
            self.set_foreground_active(session_id, false);
        }
        .instrument(tracing::info_span!(
            "turn",
            run.turn = %turn,
            cause = "background_result"
        ))
        .await;
    }

    fn finish_turn(
        &self,
        session_id: SessionId,
        turn: TurnId,
        events: &EventSink,
        result: Result<(DriveOutput, DriveStop), DeliverError>,
    ) {
        match result {
            Ok((output, _stop)) => {
                for reply in output.replies {
                    tracing::info!(name: "output", text_bytes = reply.text.len() as u64);
                    events.emit(SessionEvent::Output { text: reply.text });
                }
            }
            Err(error) => {
                let kind: &'static str = (&error).into();
                tracing::error!(name: "error", kind);
                events.emit(SessionEvent::Error {
                    message: error.to_string(),
                });
            }
        }
        if let Err(error) = self.checkpoint_session_runtime(session_id) {
            tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
            events.emit(SessionEvent::Error {
                message: error.to_string(),
            });
        }
        events.emit(SessionEvent::TurnEnded { turn });
    }

    async fn drive_one_input(
        &self,
        session_id: SessionId,
        text: String,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        // The instance is checked out of the map so it can be driven without
        // holding a `RefCell` borrow across `.await`. `InstanceSlot` is an RAII
        // guard: it reinserts the (possibly mutated) instance on every exit path.
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();

        if let Some(pending) = instance.active_approval() {
            let control = DriveControl::new();
            self.set_active_control(session_id, Some(control.clone()));
            let result = self
                .resolve_pending_approval(session_id, instance, pending, &text, &control, events)
                .await;
            self.set_active_control(session_id, None);
            return result;
        }

        instance.deliver(text)?;
        self.drive_root_ready_in_slot(session_id, instance, events)
            .await
    }

    async fn drive_root_ready(
        &self,
        session_id: SessionId,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();
        self.drive_root_ready_in_slot(session_id, instance, events)
            .await
    }

    async fn drive_root_ready_in_slot(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<Filesystem, Http, Timer>,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let control = DriveControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let (mut output, stop) = instance.drive_root_turn(&control, events).await;
        self.set_active_control(session_id, None);
        if stop == DriveStop::Cancelled {
            instance.cancel_all(CancelReason::UserRequested);
            let cleanup_control = DriveControl::new();
            cleanup_control.request_cancel();
            let (cleanup_output, _) = instance.drive_cancelled(&cleanup_control, events).await;
            output.absorb(cleanup_output);
            tracing::warn!(name: "cancelled_cleanup", "");
        }
        Ok((output, stop))
    }

    async fn drive_background(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let Some(mut slot) = self.checkout_existing_instance(session_id) else {
            return;
        };
        let control = DriveControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let stop = slot
            .get_mut()
            .drive_background_until_root_ready(&control, &events)
            .await;
        self.set_active_control(session_id, None);
        if stop == DriveStop::Cancelled {
            slot.get_mut().cancel_all(CancelReason::UserRequested);
            let cleanup_control = DriveControl::new();
            cleanup_control.request_cancel();
            let _ = slot
                .get_mut()
                .drive_cancelled(&cleanup_control, &events)
                .await;
        }
    }

    async fn finish_closing_session(&self, session_id: SessionId) {
        let (events, should_cancel, deleted) = {
            let drives = self.drives.borrow();
            let Some(drive) = drives.get(&session_id) else {
                return;
            };
            (
                drive.events.clone(),
                drive.close_cancels,
                !self.sessions.contains(session_id),
            )
        };

        let cleanup_events = match events.clone() {
            Some(events) => events,
            None => EventSink::disabled(),
        };
        if should_cancel {
            if let Some(mut slot) = self.checkout_existing_instance(session_id) {
                slot.get_mut().cancel_all(CancelReason::UserRequested);
                let control = DriveControl::new();
                control.request_cancel();
                self.set_active_control(session_id, Some(control.clone()));
                let _ = slot
                    .get_mut()
                    .drive_cancelled(&control, &cleanup_events)
                    .await;
                self.set_active_control(session_id, None);
            }
        }

        if let Some(events) = events {
            tracing::info!(name: "closed", "");
            events.emit(SessionEvent::Closed);
        }

        if deleted {
            tracing::info_span!("session.delete", run.session = %session_id).in_scope(|| {
                tracing::info!(name: "runtime_state_removed", "");
            });
            self.drives.borrow_mut().remove(&session_id);
            self.instances.borrow_mut().remove(&session_id);
            return;
        }

        if let Some(drive) = self.drives.borrow_mut().get_mut(&session_id) {
            drive.events = None;
            let _ = drive.take_pending_input();
            drive.running = false;
            drive.foreground_active = false;
            drive.control = None;
            drive.requested_control = None;
            drive.closing = false;
            drive.close_cancels = false;
        }
        if let Err(error) = self.checkpoint_session_runtime(session_id) {
            tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
        }
    }

    async fn resolve_pending_approval(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<Filesystem, Http, Timer>,
        pending: PendingApproval,
        user_reply: &str,
        control: &DriveControl,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let resolution = match approval::resolve_permission_reply::<Http, Timer>(
            self.approval_llm_config.clone(),
            &pending.summary,
            user_reply,
            control,
        )
        .await
        {
            Ok(resolution) => resolution,
            Err(ApprovalResolverError::Cancelled) => {
                instance.cancel_all(CancelReason::UserRequested);
                let cleanup_control = DriveControl::new();
                cleanup_control.request_cancel();
                let (cleanup_output, _) = instance.drive_cancelled(&cleanup_control, events).await;
                return Ok((cleanup_output, DriveStop::Cancelled));
            }
            Err(error) => return Err(error.into()),
        };

        let Some(decision) = resolution.clone().into_decision() else {
            let PermissionReplyResolution::Clarify(message) = resolution else {
                unreachable!("non-clarification resolutions map to approval decisions")
            };
            tracing::info!(name: "approval_clarification", reason = "clarify");
            return Ok((
                DriveOutput {
                    replies: vec![RootReply {
                        session: session_id,
                        text: message,
                    }],
                },
                DriveStop::Quiescent,
            ));
        };

        let decision_name: &'static str = (&decision).into();
        tracing::info!(name: "approval_resolved", decision = decision_name);
        instance
            .resolve_active_approval(decision)
            .map_err(DeliverError::from)?;
        Ok(instance.drive_root_turn(control, events).await)
    }

    fn instance_work(&self, session_id: SessionId) -> InstanceWork {
        match self.instances.borrow().get(&session_id) {
            Some(instance) => instance.work(),
            None => InstanceWork::None,
        }
    }

    fn instance_has_root_work(&self, session_id: SessionId) -> bool {
        self.instances
            .borrow()
            .get(&session_id)
            .is_some_and(|instance| instance.work() == InstanceWork::Root)
    }

    /// Check the session's agent graph out of the map (building a fresh instance
    /// when the session has none yet), wrapped in an [`InstanceSlot`] that
    /// reinserts it on drop.
    fn checkout_instance(
        &self,
        session_id: SessionId,
    ) -> InstanceSlot<'_, Filesystem, Http, Timer> {
        let instance = match self.instances.borrow_mut().remove(&session_id) {
            Some(instance) => instance,
            None => OrchestratorInstance::new(
                session_id,
                Arc::clone(&self.factory),
                self.state.get().agent_id_allocator.clone(),
                OrchestratorInstanceState::default(),
            ),
        };
        InstanceSlot {
            engine: self,
            session_id,
            instance: Some(instance),
        }
    }

    fn checkout_existing_instance(
        &self,
        session_id: SessionId,
    ) -> Option<InstanceSlot<'_, Filesystem, Http, Timer>> {
        let instance = self.instances.borrow_mut().remove(&session_id)?;
        Some(InstanceSlot {
            engine: self,
            session_id,
            instance: Some(instance),
        })
    }

    fn put_instance(
        &self,
        session_id: SessionId,
        instance: OrchestratorInstance<Filesystem, Http, Timer>,
    ) {
        if !self.sessions.contains(session_id) {
            return;
        }
        self.instances.borrow_mut().insert(session_id, instance);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum InstanceWork {
    None,
    Root,
    Background,
}

/// RAII checkout of a session's [`OrchestratorInstance`]: holds the instance out
/// of the map while it is driven and reinserts it on drop, so no exit path (an
/// early `?`, a panic while driving, or normal return) can drop the graph.
struct InstanceSlot<'a, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    engine: &'a Engine<Filesystem, Http, Timer>,
    session_id: SessionId,
    instance: Option<OrchestratorInstance<Filesystem, Http, Timer>>,
}

impl<Filesystem, Http, Timer> InstanceSlot<'_, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn get_mut(&mut self) -> &mut OrchestratorInstance<Filesystem, Http, Timer> {
        // Invariant: `instance` is `Some` for the whole lifetime of the slot;
        // it is only taken in `Drop`, after which the slot is unreachable.
        self.instance
            .as_mut()
            .expect("InstanceSlot holds its instance until Drop")
    }
}

impl<Filesystem, Http, Timer> Drop for InstanceSlot<'_, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn drop(&mut self) {
        if let Some(instance) = self.instance.take() {
            self.engine.put_instance(self.session_id, instance);
        }
    }
}

/// One wakeup of the engine's run loop: either an in-flight drive finished or the
/// command channel produced an item (`None` == closed).
enum EngineEvent {
    DriveDone,
    Command(Option<Command>),
}

/// Polls the command receiver first, then every in-flight drive future (rotating
/// the queue and dropping any that finished). Control commands must not sit
/// behind a drive future that keeps cooperatively waking itself.
struct EnginePoll<'a> {
    inflight: &'a mut VecDeque<DriveFuture>,
    recv: Option<Pin<&'a mut Receiver<Command>>>,
}

impl Future for EnginePoll<'_> {
    type Output = EngineEvent;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        if let Some(receiver) = self.recv.as_mut() {
            if let Poll::Ready(command) = receiver.as_mut().poll_next(context) {
                return Poll::Ready(EngineEvent::Command(command));
            }
        }

        let count = self.inflight.len();
        for _ in 0..count {
            let Some(mut future) = self.inflight.pop_front() else {
                break;
            };
            if future.as_mut().poll(context).is_ready() {
                return Poll::Ready(EngineEvent::DriveDone);
            }
            self.inflight.push_back(future);
        }

        Poll::Pending
    }
}
