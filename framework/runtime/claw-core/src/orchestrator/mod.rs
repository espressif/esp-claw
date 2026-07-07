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
use std::collections::{HashMap, VecDeque};
use std::rc::Rc;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};

use async_channel::{Receiver, Sender};
use claw_api::ClawApiConfig;
use claw_interface::{
    ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, CoreAffinity, Priority, WorkerHandle,
};
use claw_tool::{SharedContext, ToolRegistry};
use futures_core::Stream;

use crate::agent::{
    AgentIdAllocator, ApprovalDecision, CancelReason, FsAgentFactory, FsAgentFactoryError,
};
use crate::event::{EventSink, SessionEvent, TurnCause};
use crate::session::{DeliverError, SessionId, SessionStore};

pub use self::instance::{DriveOutput, RootReply};

use self::approval::{ApprovalResolverError, PermissionReplyResolution};
use self::control::{DriveControl, DriveStop};
use self::instance::{OrchestratorInstance, PendingApproval};

/// Stack for the orchestrator's single drive worker. It runs the whole agent
/// graph (context building, LLM round-trips, tool calls), so it matches the
/// device agent worker's budget.
const ENGINE_WORKER_STACK_SIZE: usize = 64 * 1024;

/// A per-session drive future owned by the engine's run loop while it is in
/// flight. `!Send`/`!'static`-free: it borrows nothing outside the `Rc<Engine>`
/// it captures.
type DriveFuture = Pin<Box<dyn Future<Output = ()>>>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ControlOp {
    Interrupt,
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
struct SubmittedInput {
    text: String,
    /// Type-erased per-submit context installed (via [`claw_tool::with_context`])
    /// for the duration of this turn's drive, so tool handlers can read it back.
    context: Option<SharedContext>,
}

#[derive(Default)]
struct SessionDrive {
    events: Option<EventSink>,
    pending_input: Option<SubmittedInput>,
    running: bool,
    foreground_active: bool,
    control: Option<DriveControl>,
    requested_control: Option<ControlOp>,
    closing: bool,
}

/// A command the [`Orchestrator`] handle sends to its engine worker.
///
/// Session create/list live entirely on the handle (`SessionStore` is shared and
/// synchronous), so they need no command. `CloseSession` is a command because
/// the engine must cancel live work, close the event stream, and drop the agent
/// graph. `Stop` lets shutdown drain in-flight drives and exit the run loop so
/// the worker joins.
enum Command {
    OpenSession {
        session: SessionId,
        events: EventSink,
        ack: Sender<Result<(), OpenSessionError>>,
    },
    Submit {
        session: SessionId,
        text: String,
        context: Option<SharedContext>,
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
        self.submit_with_context(text, None).await
    }

    /// Submit one user input and install per-turn tool context while it drives.
    pub async fn submit_with_context(
        &self,
        text: impl Into<String>,
        context: Option<SharedContext>,
    ) -> Result<(), SessionControlError> {
        let text = text.into();
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::Submit {
                session: self.session,
                text,
                context,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        ack_rx
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
    }

    /// Gracefully stop the current foreground drive at the next safe boundary.
    pub async fn interrupt(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Interrupt).await
    }

    /// Hard-cancel foreground and background work in this session.
    pub async fn cancel(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Cancel).await
    }

    /// Close this session: cancel live work, close the event stream, and remove
    /// the session from the registry.
    pub async fn close_session(&self) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::CloseSession {
                session: self.session,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        ack_rx
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
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
        ack_rx
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
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
    ExtractionLlm(String),
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(String),
    /// The drive worker thread could not be spawned or reported a build failure.
    #[error("failed to start the orchestrator worker: {0}")]
    Worker(String),
}

impl From<FsAgentFactoryError> for OrchestratorBuildError {
    fn from(error: FsAgentFactoryError) -> Self {
        match error {
            FsAgentFactoryError::MissingPersistenceDir => Self::MissingPersistenceDir,
            FsAgentFactoryError::ExtractionLlm(message) => Self::ExtractionLlm(message),
            FsAgentFactoryError::LongTermInit(source) => Self::LongTermInit(source.to_string()),
        }
    }
}

/// A `Send + Sync` handle to a running orchestrator.
///
/// Cloning is intentionally not provided: the handle owns the worker's lifetime
/// and joins it on drop. Wrap it in an `Arc` to share.
pub struct Orchestrator {
    /// The session id truth source, shared with the engine so the handle can
    /// validate submits/deletes synchronously without a round-trip.
    sessions: Arc<SessionStore>,
    /// Outbound command channel to the engine worker.
    command_tx: Sender<Command>,
    /// The drive worker, joined on drop. `Mutex<Option<..>>` so `Drop` can take
    /// it out behind a shared borrow.
    worker: Mutex<Option<WorkerHandle>>,
}

impl Orchestrator {
    /// Build an orchestrator: spawn the drive worker (via `T::spawn_worker`),
    /// construct the engine inside it, and wait for it to report readiness.
    ///
    /// `llm_config` is cloned into every agent, `persistence_dir` is the storage
    /// root the engine's factory owns, and `skill_roots` are the priority-ordered
    /// skill directories every agent's catalog is built from.
    ///
    /// # Errors
    ///
    /// Returns [`OrchestratorBuildError`] when the worker cannot be spawned or the
    /// engine (factory) cannot be assembled inside it.
    pub fn new<F, H, Timer, T, E>(
        tools: Arc<ToolRegistry>,
        llm_config: ClawApiConfig,
        persistence_dir: &str,
        skill_roots: &[String],
    ) -> Result<Self, OrchestratorBuildError>
    where
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
        T: ClawThread,
        E: ClawExecutor + 'static,
    {
        let sessions = Arc::new(SessionStore::new());
        let (command_tx, command_rx) = async_channel::unbounded();
        let (ready_tx, ready_rx) = mpsc::channel();

        let persistence_dir = persistence_dir.to_string();
        let skill_roots = skill_roots.to_vec();
        let sessions_engine = Arc::clone(&sessions);
        let worker = T::spawn_worker(
            "claw_orchestrator",
            ENGINE_WORKER_STACK_SIZE,
            Priority::Normal,
            CoreAffinity::Any,
            move || {
                run_engine::<F, H, Timer, E>(
                    tools,
                    llm_config,
                    persistence_dir,
                    skill_roots,
                    sessions_engine,
                    command_rx,
                    ready_tx,
                );
            },
        )
        .map_err(|error| OrchestratorBuildError::Worker(error.to_string()))?;

        match ready_rx.recv() {
            Ok(Ok(())) => Ok(Self {
                sessions,
                command_tx,
                worker: Mutex::new(Some(worker)),
            }),
            Ok(Err(error)) => {
                worker.join();
                Err(error)
            }
            Err(_) => {
                worker.join();
                Err(OrchestratorBuildError::Worker(
                    "worker exited before signalling readiness".to_string(),
                ))
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
        if !self.sessions.contains(session_id) {
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
            .map_err(|_| OpenSessionError::WorkerStopped)?;
        match ack_rx.recv_blocking() {
            Ok(Ok(())) => Ok((
                SessionControl::new(session_id, self.command_tx.clone()),
                SessionEventStream::new(receiver),
            )),
            Ok(Err(error)) => Err(error),
            Err(_) => Err(OpenSessionError::WorkerStopped),
        }
    }

    /// Create a fresh isolated conversation session.
    pub fn session_create(&self) -> SessionId {
        self.sessions.create()
    }

    /// The live conversation sessions, sorted by id.
    pub fn session_list(&self) -> Vec<SessionId> {
        let mut sessions = self.sessions.list();
        sessions.sort_by_key(|id| id.0);
        sessions
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
fn run_engine<F, H, Timer, E>(
    tools: Arc<ToolRegistry>,
    llm_config: ClawApiConfig,
    persistence_dir: String,
    skill_roots: Vec<String>,
    sessions: Arc<SessionStore>,
    command_rx: Receiver<Command>,
    ready: mpsc::Sender<Result<(), OrchestratorBuildError>>,
) where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
    E: ClawExecutor,
{
    let engine = match Engine::<F, H, Timer>::new(
        tools,
        llm_config,
        &persistence_dir,
        &skill_roots,
        sessions,
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
    E::block_on(engine.run(command_rx));
}

/// The `!Send` drive engine. Sole owner of the session instances and their drive
/// state; runs on one worker thread and multiplexes every live session.
struct Engine<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Builds agents for every session's registry.
    factory: Arc<FsAgentFactory<F, H, Timer>>,
    /// Config for the one-shot natural-language approval resolver.
    approval_llm_config: ClawApiConfig,
    /// Global agent-id allocator shared by every per-session registry.
    next_agent_id: AgentIdAllocator,
    /// One isolated agent graph per session. Single-owner interior mutability
    /// (`RefCell`): the engine is `!Send` and driven from one thread.
    instances: RefCell<HashMap<SessionId, OrchestratorInstance<F, H, Timer>>>,
    /// Per-session connection, accepted input slot, and active drive control state.
    drives: RefCell<HashMap<SessionId, SessionDrive>>,
    /// Session id truth source, shared with the handle.
    sessions: Arc<SessionStore>,
}

impl<F, H, Timer> Engine<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn new(
        tools: Arc<ToolRegistry>,
        llm_config: ClawApiConfig,
        persistence_dir: &str,
        skill_roots: &[String],
        sessions: Arc<SessionStore>,
    ) -> Result<Self, OrchestratorBuildError> {
        let factory = Arc::new(FsAgentFactory::<F, H, Timer>::new(
            tools,
            llm_config.clone(),
            persistence_dir,
            skill_roots,
        )?);
        Ok(Self {
            factory,
            approval_llm_config: llm_config,
            next_agent_id: AgentIdAllocator::new(),
            instances: RefCell::new(HashMap::new()),
            drives: RefCell::new(HashMap::new()),
            sessions,
        })
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
                            | Command::CloseSession { ack, .. } => {
                                let _ = ack.try_send(Err(SessionControlError::WorkerStopped));
                            }
                            Command::Stop => {}
                        }
                    }
                }
                return;
            }
            // Once stopping, stop accepting new commands; just drain in-flight.
            let recv = (rx_open && !stopping).then(|| command_rx.as_mut());
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
                    let result = self.open_session_stream(session, events);
                    let _ = ack.try_send(result);
                }
                EngineEvent::Command(Some(Command::Submit {
                    session,
                    text,
                    context,
                    ack,
                })) => {
                    let (result, future) =
                        self.submit_input(session, SubmittedInput { text, context });
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
                EngineEvent::Command(Some(Command::Stop)) => stopping = true,
                EngineEvent::Command(None) => rx_open = false,
            }
        }
    }

    fn open_session_stream(
        &self,
        session_id: SessionId,
        events: EventSink,
    ) -> Result<(), OpenSessionError> {
        if !self.sessions.contains(session_id) {
            return Err(OpenSessionError::SessionNotFound(session_id));
        }
        let mut drives = self.drives.borrow_mut();
        let drive = drives.entry(session_id).or_default();
        if drive.events.is_some() && !drive.closing {
            return Err(OpenSessionError::AlreadyOpen(session_id));
        }
        drive.events = Some(events);
        drive.closing = false;
        Ok(())
    }

    fn submit_input(
        self: &Rc<Self>,
        session_id: SessionId,
        input: SubmittedInput,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        if !self.sessions.contains(session_id) {
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if drive.pending_input.is_some()
                || drive.foreground_active
                || self.instance_has_root_work(session_id)
            {
                return (Err(SessionControlError::Busy(session_id)), None);
            }
            drive.pending_input = Some(input);
            if let Some(control) = &drive.control {
                control.request_wake();
            }
        }
        (Ok(()), self.ensure_session_drive(session_id))
    }

    fn control_session(
        self: &Rc<Self>,
        session_id: SessionId,
        op: ControlOp,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        if !self.sessions.contains(session_id) {
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if op == ControlOp::Cancel {
                drive.pending_input = None;
            }
            drive.requested_control = Some(ControlOp::merge(drive.requested_control, op));
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
        }
        (Ok(()), self.ensure_session_drive(session_id))
    }

    fn close_session(
        self: &Rc<Self>,
        session_id: SessionId,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let existed = self.sessions.delete(session_id);
        let mut should_start = false;
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                drop(drives);
                self.instances.borrow_mut().remove(&session_id);
                let result = if existed {
                    Ok(())
                } else {
                    Err(SessionControlError::SessionClosed(session_id))
                };
                return (result, None);
            };
            drive.closing = true;
            drive.pending_input = None;
            drive.requested_control = Some(ControlOp::Cancel);
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
            if !drive.running {
                drive.running = true;
                should_start = true;
            }
        }
        let future = should_start.then(|| {
            let engine = Rc::clone(self);
            Box::pin(async move {
                engine.drive_session(session_id).await;
            }) as DriveFuture
        });
        (Ok(()), future)
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
        Some(Box::pin(async move {
            engine.drive_session(session_id).await;
        }))
    }

    async fn drive_session(&self, session_id: SessionId) {
        loop {
            if self.session_is_closing(session_id) {
                self.cancel_and_close_session(session_id).await;
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
        drive.pending_input.take()
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
        self.set_foreground_active(session_id, true);
        events.emit(SessionEvent::TurnStarted {
            cause: TurnCause::UserSubmit,
        });
        let drive = claw_tool::with_context(
            input.context,
            self.drive_one_input(session_id, input.text, &events),
        );
        self.finish_turn(session_id, &events, drive.await);
        self.set_foreground_active(session_id, false);
    }

    async fn drive_background_result_turn(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        self.set_foreground_active(session_id, true);
        events.emit(SessionEvent::TurnStarted {
            cause: TurnCause::BackgroundResult,
        });
        let result = self.drive_root_ready(session_id, &events).await;
        self.finish_turn(session_id, &events, result);
        self.set_foreground_active(session_id, false);
    }

    fn finish_turn(
        &self,
        session_id: SessionId,
        events: &EventSink,
        result: Result<(DriveOutput, DriveStop), DeliverError>,
    ) {
        match result {
            Ok((output, _stop)) => {
                for reply in output.replies {
                    events.emit(SessionEvent::Output { text: reply.text });
                }
            }
            Err(error) => {
                events.emit(SessionEvent::Error {
                    message: error.to_string(),
                });
            }
        }
        events.emit(SessionEvent::TurnEnded);
        let _ = session_id;
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
        let turn = instance.next_turn();
        // session > turn: the session span opens `conversation.session`, the
        // turn span opens `conversation.turn`. Every agent/iteration/tool span
        // produced while driving nests under them, so one drive reads as a unit.
        let _session_span =
            tracing::info_span!("session", conversation.session = %session_id).entered();
        let _turn_span =
            tracing::info_span!("turn", conversation.turn = turn, cause = "message",).entered();

        if let Some(pending) = instance.active_approval() {
            let control = DriveControl::new();
            self.set_active_control(session_id, Some(control.clone()));
            let result = self
                .resolve_pending_approval(session_id, instance, pending, &text, &control, events)
                .await;
            self.set_active_control(session_id, None);
            return result;
        }

        instance.deliver(text).map_err(DeliverError::Agent)?;
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
        let turn = instance.next_turn();
        let _session_span =
            tracing::info_span!("session", conversation.session = %session_id).entered();
        let _turn_span = tracing::info_span!(
            "turn",
            conversation.turn = turn,
            cause = "background_result",
        )
        .entered();
        self.drive_root_ready_in_slot(session_id, instance, events)
            .await
    }

    async fn drive_root_ready_in_slot(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<F, H, Timer>,
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

    async fn cancel_and_close_session(&self, session_id: SessionId) {
        if let Some(events) = self.session_events(session_id) {
            if let Some(mut slot) = self.checkout_existing_instance(session_id) {
                slot.get_mut().cancel_all(CancelReason::UserRequested);
                let control = DriveControl::new();
                control.request_cancel();
                self.set_active_control(session_id, Some(control.clone()));
                let _ = slot.get_mut().drive_cancelled(&control, &events).await;
                self.set_active_control(session_id, None);
            }
            events.emit(SessionEvent::Closed);
        }
        self.drives.borrow_mut().remove(&session_id);
        self.instances.borrow_mut().remove(&session_id);
    }

    async fn resolve_pending_approval(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<F, H, Timer>,
        pending: PendingApproval,
        user_reply: &str,
        control: &DriveControl,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let resolution = match approval::resolve_permission_reply::<H, Timer>(
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
            Err(error) => return Err(DeliverError::Agent(error.to_string())),
        };

        let Some(decision) = resolution.clone().into_decision() else {
            let PermissionReplyResolution::Clarify(message) = resolution else {
                unreachable!("non-clarification resolutions map to approval decisions")
            };
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

        let decision_label = match &decision {
            ApprovalDecision::Approved => "approved",
            ApprovalDecision::Rejected(_) => "rejected",
        };
        tracing::info!(
            session = %session_id,
            agent = %pending.agent,
            approval = %pending.approval,
            decision = decision_label,
            "approval resolved from user reply"
        );
        instance
            .resolve_active_approval(decision)
            .map_err(|error| DeliverError::Agent(error.to_string()))?;
        Ok(instance.drive_root_turn(control, events).await)
    }

    fn instance_work(&self, session_id: SessionId) -> InstanceWork {
        self.instances
            .borrow()
            .get(&session_id)
            .map(OrchestratorInstance::work)
            .unwrap_or(InstanceWork::None)
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
    fn checkout_instance(&self, session_id: SessionId) -> InstanceSlot<'_, F, H, Timer> {
        let instance = self
            .instances
            .borrow_mut()
            .remove(&session_id)
            .unwrap_or_else(|| {
                OrchestratorInstance::new(
                    session_id,
                    Arc::clone(&self.factory),
                    self.next_agent_id.clone(),
                )
            });
        InstanceSlot {
            engine: self,
            session_id,
            instance: Some(instance),
        }
    }

    fn checkout_existing_instance(
        &self,
        session_id: SessionId,
    ) -> Option<InstanceSlot<'_, F, H, Timer>> {
        let instance = self.instances.borrow_mut().remove(&session_id)?;
        Some(InstanceSlot {
            engine: self,
            session_id,
            instance: Some(instance),
        })
    }

    fn put_instance(&self, session_id: SessionId, instance: OrchestratorInstance<F, H, Timer>) {
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
struct InstanceSlot<'a, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    engine: &'a Engine<F, H, Timer>,
    session_id: SessionId,
    instance: Option<OrchestratorInstance<F, H, Timer>>,
}

impl<F, H, Timer> InstanceSlot<'_, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn get_mut(&mut self) -> &mut OrchestratorInstance<F, H, Timer> {
        // Invariant: `instance` is `Some` for the whole lifetime of the slot;
        // it is only taken in `Drop`, after which the slot is unreachable.
        self.instance
            .as_mut()
            .expect("InstanceSlot holds its instance until Drop")
    }
}

impl<F, H, Timer> Drop for InstanceSlot<'_, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
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
