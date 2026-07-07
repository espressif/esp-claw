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
use crate::event::{AgentEvent, EventSink};
use crate::session::{DeliverError, DeliveryKind, SessionError, SessionId, SessionStore};

pub use self::instance::{DriveOutput, RootReply};

use self::approval::{ApprovalResolverError, PermissionReplyResolution};
use self::control::{DriveStop, SessionControl};
use self::instance::{OrchestratorInstance, PendingApproval};

/// Stack for the orchestrator's single drive worker. It runs the whole agent
/// graph (context building, LLM round-trips, tool calls), so it matches the
/// device agent worker's budget.
const ENGINE_WORKER_STACK_SIZE: usize = 64 * 1024;

/// A per-session drive future owned by the engine's run loop while it is in
/// flight. `!Send`/`!'static`-free: it borrows nothing outside the `Rc<Engine>`
/// it captures.
type DriveFuture = Pin<Box<dyn Future<Output = ()>>>;

/// One queued unit of delivery for a session, carrying its own event channel and
/// the invocation context to install while its turn drives.
struct QueuedSubmission {
    text: String,
    kind: DeliveryKind,
    /// This submission's event channel, wrapped as a sink. Dropping it closes the
    /// channel, which ends the paired [`SubmitStream`].
    events: EventSink,
    /// Type-erased per-submission context installed (via [`claw_tool::with_context`])
    /// for the duration of this turn's drive, so tool handlers can read it back.
    context: Option<SharedContext>,
}

#[derive(Default)]
struct SessionDrive {
    active: bool,
    control: Option<SessionControl>,
    carried: Option<QueuedSubmission>,
    pending: VecDeque<QueuedSubmission>,
}

/// A command the [`Orchestrator`] handle sends to its engine worker.
///
/// Session create/list live entirely on the handle (`SessionStore` is shared and
/// synchronous), so they need no command. `DeleteSession` is a command because
/// the engine must drop the live agent graph for that session. `Stop` lets
/// shutdown drain in-flight drives and exit the run loop so the worker joins.
enum Command {
    Submit {
        session: SessionId,
        text: String,
        kind: DeliveryKind,
        events: EventSink,
        context: Option<SharedContext>,
    },
    DeleteSession {
        session: SessionId,
    },
    Stop,
}

/// The async stream one [`Orchestrator::submit`] returns.
///
/// One `submit` == one turn == one session, so the stream *is* that scope. It is
/// a thin wrapper over this submission's event `Receiver`: the engine worker
/// produces the events; the caller just drains them. The stream ends when this
/// submission's turn finishes (its event channel closes).
pub struct SubmitStream {
    /// This submission's event channel. `async_channel::Receiver` is `!Unpin`
    /// (it holds a pinned listener), so it is box-pinned once here.
    events: Pin<Box<Receiver<AgentEvent>>>,
}

impl SubmitStream {
    fn new(events: Receiver<AgentEvent>) -> Self {
        Self {
            events: Box::pin(events),
        }
    }
}

impl Stream for SubmitStream {
    type Item = AgentEvent;

    fn poll_next(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Option<AgentEvent>> {
        self.get_mut().events.as_mut().poll_next(context)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StartMode {
    Fresh,
    Interrupted,
    Cancelled,
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
    /// Build an orchestrator: spawn the drive worker (via `thread`), construct the
    /// engine inside it, and wait for it to report readiness.
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
        thread: &T,
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
        let worker = thread
            .spawn_worker(
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

    /// Submit one message to a live session and stream its turn as
    /// [`AgentEvent`]s.
    ///
    /// Returns immediately with a [`SubmitStream`]; the turn runs on the engine
    /// worker as the caller drains it. A submit to an unknown session yields one
    /// [`AgentEvent::Error`] and ends. `context` is a type-erased per-submission
    /// context installed while the turn drives, so tool handlers can read it back
    /// via [`claw_tool::current_context`].
    pub fn submit(
        &self,
        session_id: SessionId,
        text: String,
        kind: DeliveryKind,
        context: Option<SharedContext>,
    ) -> SubmitStream {
        let (sender, receiver) = async_channel::unbounded();
        if !self.sessions.contains(session_id) {
            let _ = sender.try_send(AgentEvent::Error {
                message: DeliverError::SessionNotFound(session_id).to_string(),
            });
            return SubmitStream::new(receiver);
        }

        let events = EventSink::new(sender);
        if let Err(error) = self.command_tx.try_send(Command::Submit {
            session: session_id,
            text,
            kind,
            events,
            context,
        }) {
            // The engine is gone: surface it on the stream we already returned.
            if let Command::Submit { events, .. } = error.into_inner() {
                events.emit(AgentEvent::Error {
                    message: "orchestrator worker is not running".to_string(),
                });
            }
        }
        SubmitStream::new(receiver)
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

    /// Delete a session and ask the engine to drop its live agent graph.
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::NotFound`] when `session_id` is not live.
    pub fn session_delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        self.sessions.delete(session_id)?;
        let _ = self
            .command_tx
            .try_send(Command::DeleteSession { session: session_id });
        Ok(())
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
    let engine = match Engine::<F, H, Timer>::new(tools, llm_config, &persistence_dir, &skill_roots, sessions)
    {
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
    /// Per-session delivery state (append/interrupt/cancel sequencing).
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

    /// Multiplex every in-flight session drive plus the command channel on one
    /// thread. Each active session is one entry in `inflight`; the command
    /// receiver is polled alongside them (same hand-rolled shape as the agent
    /// tick batch), so a new submit/interrupt/cancel is observed between the
    /// cooperative yields of any running drive.
    async fn run(self: &Rc<Self>, command_rx: Receiver<Command>) {
        let mut command_rx = core::pin::pin!(command_rx);
        let mut inflight: VecDeque<DriveFuture> = VecDeque::new();
        let mut rx_open = true;
        let mut stopping = false;

        loop {
            if (stopping || !rx_open) && inflight.is_empty() {
                if stopping {
                    // Any `Submit` still queued behind `Stop` will never be driven.
                    // Drain them non-blockingly and end each paired `SubmitStream`
                    // with an error, instead of letting the stream close silently
                    // (no `TurnStarted`/`TurnEnded`) when the channel drops at
                    // teardown. Non-blocking so we never wait on the channel to
                    // close — that only happens once the handle drops, which joins
                    // this worker first.
                    while let Ok(command) = command_rx.try_recv() {
                        if let Command::Submit { events, .. } = command {
                            events.emit(AgentEvent::Error {
                                message: "orchestrator is shutting down".to_string(),
                            });
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
                EngineEvent::Command(Some(Command::Submit {
                    session,
                    text,
                    kind,
                    events,
                    context,
                })) => {
                    if let Some(future) = self.begin_submission(
                        session,
                        QueuedSubmission {
                            text,
                            kind,
                            events,
                            context,
                        },
                    ) {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::DeleteSession { session })) => {
                    self.drop_session(session);
                }
                EngineEvent::Command(Some(Command::Stop)) => stopping = true,
                EngineEvent::Command(None) => rx_open = false,
            }
        }
    }

    /// Queue `submission` for `session_id`. When the session has no active drive,
    /// start one and return its future for the run loop to poll; otherwise fold
    /// the submission into the active drive per its [`DeliveryKind`] and return
    /// `None`.
    fn begin_submission(
        self: &Rc<Self>,
        session_id: SessionId,
        submission: QueuedSubmission,
    ) -> Option<DriveFuture> {
        let mut drives = self.drives.borrow_mut();
        let drive = drives.entry(session_id).or_default();
        if drive.active {
            match submission.kind {
                DeliveryKind::Append => drive.pending.push_back(submission),
                DeliveryKind::Interrupt if drive.carried.is_none() => {
                    if let Some(control) = &drive.control {
                        control.request_interrupt();
                    }
                    drive.carried = Some(submission);
                }
                DeliveryKind::Interrupt => {
                    drive.pending.push_back(submission);
                }
                DeliveryKind::Cancel => {
                    if let Some(control) = &drive.control {
                        control.request_cancel();
                    }
                    if let Some(replaced) = drive.carried.replace(submission) {
                        replaced.events.emit(AgentEvent::Error {
                            message: DeliverError::Superseded.to_string(),
                        });
                    }
                }
            }
            None
        } else {
            drive.active = true;
            drive.pending.push_back(submission);
            drop(drives);
            let engine = Rc::clone(self);
            Some(Box::pin(async move {
                engine.drive_submissions(session_id).await;
            }))
        }
    }

    async fn drive_submissions(&self, session_id: SessionId) {
        let mut mode = StartMode::Fresh;
        loop {
            let Some(submission) = self.take_next_submission(session_id) else {
                self.finish_drive(session_id);
                return;
            };
            let QueuedSubmission {
                text,
                kind,
                events,
                context,
            } = submission;
            events.emit(AgentEvent::TurnStarted);
            // Install this submission's context for the duration of its turn so
            // tool handlers deep in the drive can read it back.
            let drive = claw_tool::with_context(
                context,
                self.drive_one_submission(session_id, text, kind, mode, &events),
            );
            let stop = match drive.await {
                Ok((output, stop)) => {
                    for reply in output.replies {
                        events.emit(AgentEvent::Output { text: reply.text });
                    }
                    stop
                }
                Err(error) => {
                    events.emit(AgentEvent::Error {
                        message: error.to_string(),
                    });
                    DriveStop::Quiescent
                }
            };
            events.emit(AgentEvent::TurnEnded);
            // Drop this submission's sink so its channel closes and the paired
            // stream ends; later submissions keep their own channels alive.
            drop(events);
            mode = match stop {
                DriveStop::Quiescent => StartMode::Fresh,
                DriveStop::Interrupted => StartMode::Interrupted,
                DriveStop::Cancelled => StartMode::Cancelled,
            };
        }
    }

    fn take_next_submission(&self, session_id: SessionId) -> Option<QueuedSubmission> {
        let mut drives = self.drives.borrow_mut();
        let drive = drives.get_mut(&session_id)?;
        if let Some(submission) = drive.carried.take() {
            return Some(submission);
        }
        drive.pending.pop_front()
    }

    fn finish_drive(&self, session_id: SessionId) {
        let mut drives = self.drives.borrow_mut();
        if let Some(drive) = drives.get_mut(&session_id) {
            drive.active = false;
            drive.control = None;
        }
    }

    fn set_active_control(&self, session_id: SessionId, control: Option<SessionControl>) {
        let mut drives = self.drives.borrow_mut();
        if let Some(drive) = drives.get_mut(&session_id) {
            if let Some(control) = &control {
                if let Some(submission) = &drive.carried {
                    match submission.kind {
                        DeliveryKind::Interrupt => control.request_interrupt(),
                        DeliveryKind::Cancel => control.request_cancel(),
                        DeliveryKind::Append => {}
                    }
                }
            }
            drive.control = control;
        }
    }

    async fn drive_one_submission(
        &self,
        session_id: SessionId,
        text: String,
        kind: DeliveryKind,
        mode: StartMode,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }

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
        let _turn_span = tracing::info_span!(
            "turn",
            conversation.turn = turn,
            cause = "message",
            delivery_kind = ?kind
        )
        .entered();

        if kind == DeliveryKind::Cancel {
            instance.cancel_active_approval(CancelReason::Superseded);
        } else if let Some(pending) = instance.active_approval() {
            let control = SessionControl::new();
            self.set_active_control(session_id, Some(control.clone()));
            let result = self
                .resolve_pending_approval(session_id, instance, pending, &text, &control, events)
                .await;
            self.set_active_control(session_id, None);
            return result;
        }

        match mode {
            StartMode::Fresh => {
                if kind == DeliveryKind::Cancel {
                    instance.cancel_root(CancelReason::Superseded);
                }
                instance.deliver(text.clone()).map_err(DeliverError::Agent)?;
            }
            StartMode::Interrupted => {
                instance
                    .interrupt_root(text.clone())
                    .map_err(DeliverError::Agent)?;
            }
            StartMode::Cancelled => {
                instance.cancel_root(CancelReason::Superseded);
                instance.deliver(text.clone()).map_err(DeliverError::Agent)?;
            }
        }

        let control = SessionControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let output = instance.drive_interruptible(&control, events).await;
        self.set_active_control(session_id, None);
        Ok(output)
    }

    async fn resolve_pending_approval(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<F, H, Timer>,
        pending: PendingApproval,
        user_reply: &str,
        control: &SessionControl,
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
                return Ok((DriveOutput::default(), DriveStop::Cancelled));
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
                        ended: false,
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
        Ok(instance.drive_interruptible(control, events).await)
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

    fn put_instance(&self, session_id: SessionId, instance: OrchestratorInstance<F, H, Timer>) {
        if !self.sessions.contains(session_id) {
            return;
        }
        self.instances.borrow_mut().insert(session_id, instance);
    }

    /// Drop a deleted session's live agent graph and drive state.
    fn drop_session(&self, session_id: SessionId) {
        self.instances.borrow_mut().remove(&session_id);
        self.drives.borrow_mut().remove(&session_id);
    }
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

/// Polls every in-flight drive future (rotating the queue, dropping any that
/// finished) and then the command receiver, mirroring the agent tick batch's
/// hand-rolled multiplexing so the engine needs no per-session task.
struct EnginePoll<'a> {
    inflight: &'a mut VecDeque<DriveFuture>,
    recv: Option<Pin<&'a mut Receiver<Command>>>,
}

impl Future for EnginePoll<'_> {
    type Output = EngineEvent;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
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

        match self.recv.as_mut() {
            Some(receiver) => receiver.as_mut().poll_next(context).map(EngineEvent::Command),
            None => Poll::Pending,
        }
    }
}
