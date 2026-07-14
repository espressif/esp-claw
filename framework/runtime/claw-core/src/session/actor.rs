use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::rc::Rc;
use std::sync::{Arc, RwLock};

use async_channel::{Receiver, Sender};
use claw_checkpoint::DurableState;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use futures_core::Stream;
use strum::IntoStaticStr;
use tracing::Instrument as _;

use crate::agent::FsAgentFactory;
use crate::config::ClawApiManager;
use crate::multiagent::{
    AgentIdAllocator, ApprovalResolutionError, DriveControl, DriveOutput, DriveStop,
    MultiagentDeliverError, MultiagentRestoreError, MultiagentRuntime, MultiagentState,
    MultiagentWork, PendingApproval, TurnStopMode,
};
use crate::protocol::{
    EventSink, Message, SessionEvent, SessionId, SessionPersistence, StreamPart, TurnId, TurnOrigin,
};

use super::api::{
    ControlOp, OpenSessionError, SessionCommand, SessionControlError, SessionEndpoint,
};
use super::approval::{self, ApprovalResolverError, PermissionReplyResolution};
use super::persistence::{SessionCheckpointer, SessionRestore};
use super::state::SessionState;

type RuntimeFuture<Filesystem, Http, Timer> =
    Pin<Box<dyn Future<Output = RuntimeCompletion<Filesystem, Http, Timer>>>>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RuntimeDriveKind {
    Foreground,
    Background,
    Stop,
}

impl RuntimeDriveKind {
    fn is_foreground(self) -> bool {
        self == Self::Foreground
    }
}

enum RuntimeDriveResult {
    Driven(Result<(DriveOutput, DriveStop), DeliverError>),
    Stopped,
}

struct RuntimeCompletion<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    runtime: MultiagentRuntime<Filesystem, Http, Timer>,
    result: RuntimeDriveResult,
}

enum RuntimeExecution<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    Idle(MultiagentRuntime<Filesystem, Http, Timer>),
    Driving {
        kind: RuntimeDriveKind,
        control: DriveControl,
        future: RuntimeFuture<Filesystem, Http, Timer>,
    },
}

#[derive(Debug, IntoStaticStr, thiserror::Error)]
enum DeliverError {
    #[strum(serialize = "agent")]
    #[error(transparent)]
    Multiagent(#[from] MultiagentDeliverError),
    #[strum(serialize = "agent")]
    #[error(transparent)]
    ApprovalResolver(#[from] ApprovalResolverError),
    #[strum(serialize = "agent")]
    #[error(transparent)]
    ApprovalResolution(#[from] ApprovalResolutionError),
}

pub(crate) enum SessionActorExit {
    Deleted(SessionId),
    Shutdown(SessionId),
}

impl SessionActorExit {
    pub(crate) fn session(&self) -> SessionId {
        match self {
            Self::Deleted(session) | Self::Shutdown(session) => *session,
        }
    }
}

/// The sole owner of one session's turn state, event stream, and agent graph.
pub(crate) struct SessionActor<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    session: SessionId,
    persistence: SessionPersistence,
    state: DurableState<SessionState>,
    execution: Option<RuntimeExecution<Filesystem, Http, Timer>>,
    checkpointer: SessionCheckpointer<Filesystem>,
    api_manager: Arc<RwLock<ClawApiManager>>,
    events: Option<EventSink>,
    active_lease: Option<u64>,
    next_lease: u64,
    announced_turn: Option<TurnId>,
    checkpoint_pending: bool,
    requested_control: Option<ControlOp>,
    control_acks: Vec<Sender<Result<(), SessionControlError>>>,
    close_requested: bool,
    close_acks: Vec<Sender<Result<(), SessionControlError>>>,
    delete_requested: bool,
    delete_acks: Vec<Sender<Result<(), SessionControlError>>>,
    shutdown_requested: bool,
}

impl<Filesystem, Http, Timer> SessionActor<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn fresh(
        session: SessionId,
        persistence: SessionPersistence,
        factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_ids: AgentIdAllocator,
        checkpointer: SessionCheckpointer<Filesystem>,
        api_manager: Arc<RwLock<ClawApiManager>>,
    ) -> Self {
        let runtime =
            MultiagentRuntime::new(session, factory, agent_ids, MultiagentState::default());
        Self::new(
            session,
            persistence,
            SessionState::default(),
            runtime,
            checkpointer,
            api_manager,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn restored(
        session: SessionId,
        persistence: SessionPersistence,
        factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_ids: AgentIdAllocator,
        checkpointer: SessionCheckpointer<Filesystem>,
        api_manager: Arc<RwLock<ClawApiManager>>,
        restore: SessionRestore,
    ) -> Result<Self, MultiagentRestoreError> {
        let runtime = MultiagentRuntime::from_restored_state(
            session,
            factory,
            agent_ids,
            restore.multiagent,
        )?;
        Ok(Self::new(
            session,
            persistence,
            restore.state,
            runtime,
            checkpointer,
            api_manager,
        ))
    }

    fn new(
        session: SessionId,
        persistence: SessionPersistence,
        state: SessionState,
        runtime: MultiagentRuntime<Filesystem, Http, Timer>,
        checkpointer: SessionCheckpointer<Filesystem>,
        api_manager: Arc<RwLock<ClawApiManager>>,
    ) -> Self {
        Self {
            session,
            persistence,
            state: DurableState::new(state),
            execution: Some(RuntimeExecution::Idle(runtime)),
            checkpointer,
            api_manager,
            events: None,
            active_lease: None,
            next_lease: 1,
            announced_turn: None,
            checkpoint_pending: false,
            requested_control: None,
            control_acks: Vec::new(),
            close_requested: false,
            close_acks: Vec::new(),
            delete_requested: false,
            delete_acks: Vec::new(),
            shutdown_requested: false,
        }
    }

    pub(crate) async fn run(mut self, commands: Receiver<SessionCommand>) -> SessionActorExit {
        let mut commands = Box::pin(commands);
        loop {
            if let Some(exit) = self.advance() {
                return exit;
            }

            match (ActorPoll {
                commands: commands.as_mut(),
                execution: &mut self.execution,
            })
            .await
            {
                ActorEvent::Command(Some(command)) => self.handle_command(command),
                ActorEvent::Command(None) => self.shutdown_requested = true,
                ActorEvent::RuntimeFinished { kind, result } => {
                    self.handle_runtime_finished(kind, result)
                }
            }
        }
    }

    /// Run immediate state transitions until the actor must wait for a command
    /// or one runtime operation.
    fn advance(&mut self) -> Option<SessionActorExit> {
        loop {
            if self.is_driving() {
                return None;
            }

            if self.delete_requested || self.shutdown_requested || self.close_requested {
                if self.needs_stop() {
                    self.start_stop(TurnStopMode::DeleteSpawnedAgents);
                    return None;
                }
                self.finish_active_turn();
                self.finish_control_request();
                if self.delete_requested {
                    self.emit_closed();
                    for ack in std::mem::take(&mut self.delete_acks) {
                        let _ = ack.try_send(Ok(()));
                    }
                    return Some(SessionActorExit::Deleted(self.session));
                }
                if self.shutdown_requested {
                    let _ = self.checkpoint();
                    self.emit_closed();
                    return Some(SessionActorExit::Shutdown(self.session));
                }
                self.finish_close();
                continue;
            }

            if let Some(op) = self.requested_control {
                if self.needs_stop() {
                    let mode = match op {
                        ControlOp::Interrupt => TurnStopMode::PreserveAgents,
                        ControlOp::Cancel => TurnStopMode::DeleteSpawnedAgents,
                    };
                    self.start_stop(mode);
                    return None;
                }
                self.finish_active_turn();
                self.finish_control_request();
                continue;
            }

            if self.active_lease.is_none() {
                return None;
            }

            if self.checkpoint_pending {
                if self.runtime().checkpoint_ready() {
                    self.checkpoint_pending = false;
                    if let Err(error) = self.checkpoint() {
                        self.emit_error(error.to_string());
                    }
                }
            }

            self.announce_turn();
            if let Some(input) = self.state.get_mut().take_pending_input() {
                self.start_user_input(input);
                return None;
            }

            match self.runtime().work() {
                MultiagentWork::Root => match self.state.get().active_turn_origin() {
                    None => {
                        let origin = self
                            .runtime()
                            .pending_root_origin()
                            .expect("root work outside a turn has a subagent origin");
                        self.state.get_mut().begin_subagent_turn(origin);
                    }
                    Some(TurnOrigin::User) if self.runtime().active_approval().is_some() => {
                        return None;
                    }
                    Some(TurnOrigin::User) => self.finish_active_turn(),
                    Some(TurnOrigin::Subagent { .. }) => {
                        self.start_pending_root_result();
                        return None;
                    }
                },
                MultiagentWork::Background => {
                    if self.state.get().has_active_turn()
                        && self.runtime().active_approval().is_none()
                    {
                        self.finish_active_turn();
                    } else {
                        self.start_background();
                        return None;
                    }
                }
                MultiagentWork::None => {
                    if self.runtime().active_approval().is_some() {
                        return None;
                    }
                    if self.state.get().has_active_turn() {
                        self.finish_active_turn();
                    } else {
                        return None;
                    }
                }
            }
        }
    }

    fn handle_command(&mut self, command: SessionCommand) {
        match command {
            SessionCommand::Open {
                events,
                commands,
                ack,
            } => self.open(events, commands, ack),
            SessionCommand::Submit {
                lease,
                message,
                ack,
            } => self.submit(lease, message, ack),
            SessionCommand::Control { lease, op, ack } => self.control(lease, op, ack),
            SessionCommand::SetReasoningEffort { lease, effort, ack } => {
                if self.accepts(lease) {
                    self.state.get_mut().set_reasoning_effort(effort);
                    let _ = ack.try_send(Ok(()));
                } else {
                    self.reject_closed(ack);
                }
            }
            SessionCommand::Close { lease, ack } => self.close(lease, ack),
            SessionCommand::Delete { ack } => {
                self.delete_requested = true;
                self.delete_acks.push(ack);
                self.cancel_running();
            }
            SessionCommand::Shutdown => {
                self.shutdown_requested = true;
                self.cancel_running();
            }
        }
    }

    fn open(
        &mut self,
        events: EventSink,
        commands: Sender<SessionCommand>,
        ack: Sender<Result<SessionEndpoint, OpenSessionError>>,
    ) {
        if self.active_lease.is_some()
            || self.close_requested
            || self.delete_requested
            || self.shutdown_requested
        {
            let _ = ack.try_send(Err(OpenSessionError::AlreadyOpen(self.session)));
            return;
        }
        let lease = self.next_lease;
        self.next_lease = self.next_lease.saturating_add(1);
        self.active_lease = Some(lease);
        self.events = Some(events);
        self.announced_turn = None;
        let _ = ack.try_send(Ok(SessionEndpoint::new(lease, commands)));
    }

    fn submit(&mut self, lease: u64, input: Message, ack: Sender<Result<(), SessionControlError>>) {
        let has_text = !input.as_str().is_empty();
        let text_bytes = input.as_str().len() as u64;
        if !self.accepts(lease) {
            tracing::warn!(name: "submit_rejected", reason = "session_closed");
            self.reject_closed(ack);
            return;
        }
        let foreground_running = self
            .driving_kind()
            .is_some_and(RuntimeDriveKind::is_foreground);
        let continues_approval = !foreground_running
            && self.state.get().has_active_turn()
            && self
                .idle_runtime()
                .is_some_and(|runtime| runtime.active_approval().is_some());
        let root_busy = self
            .idle_runtime()
            .is_some_and(|runtime| runtime.work() == MultiagentWork::Root);
        if foreground_running
            || self.state.get().has_pending_input()
            || (self.state.get().has_active_turn() && !continues_approval)
            || root_busy
        {
            tracing::warn!(name: "submit_rejected", reason = "busy", has_text, text_bytes);
            let _ = ack.try_send(Err(SessionControlError::Busy(self.session)));
            return;
        }
        if continues_approval {
            self.state.get_mut().continue_turn(input);
        } else {
            self.state.get_mut().begin_user_turn(input);
        }
        self.checkpoint_pending = true;
        if let Some(control) = self.driving_control() {
            control.request_wake();
        }
        tracing::info!(name: "submit_accepted", has_text, text_bytes);
        let _ = ack.try_send(Ok(()));
    }

    fn control(&mut self, lease: u64, op: ControlOp, ack: Sender<Result<(), SessionControlError>>) {
        if !self.accepts(lease) {
            self.reject_closed(ack);
            return;
        }
        let has_active_turn = self.state.get().has_active_turn();
        let has_background = self
            .driving_kind()
            .is_some_and(|kind| kind == RuntimeDriveKind::Background)
            || self
                .idle_runtime()
                .is_some_and(|runtime| runtime.work() == MultiagentWork::Background);
        if !has_active_turn && (op == ControlOp::Interrupt || !has_background) {
            let _ = ack.try_send(Ok(()));
            return;
        }
        self.requested_control = Some(ControlOp::merge(self.requested_control, op));
        self.control_acks.push(ack);
        if let Some(control) = self.driving_control() {
            match self.requested_control {
                Some(ControlOp::Interrupt) => control.request_interrupt(),
                Some(ControlOp::Cancel) => control.request_cancel(),
                None => {}
            }
        }
    }

    fn close(&mut self, lease: u64, ack: Sender<Result<(), SessionControlError>>) {
        if !self.accepts(lease) {
            self.reject_closed(ack);
            return;
        }
        self.close_requested = true;
        self.close_acks.push(ack);
        self.cancel_running();
    }

    fn accepts(&self, lease: u64) -> bool {
        self.active_lease == Some(lease)
            && !self.delete_requested
            && !self.shutdown_requested
            && !self.close_requested
    }

    fn reject_closed(&self, ack: Sender<Result<(), SessionControlError>>) {
        let _ = ack.try_send(Err(SessionControlError::SessionClosed(self.session)));
    }

    fn needs_stop(&self) -> bool {
        self.state.get().has_active_turn()
            || self
                .idle_runtime()
                .is_some_and(|runtime| runtime.work() != MultiagentWork::None)
    }

    fn cancel_running(&self) {
        if let Some(control) = self.driving_control() {
            control.request_cancel();
        }
    }

    fn announce_turn(&mut self) {
        let Some(turn) = self.state.get().active_turn_id() else {
            return;
        };
        if self.announced_turn == Some(turn) {
            return;
        }
        let origin = self
            .state
            .get()
            .active_turn_origin()
            .expect("an active turn has an origin");
        self.announced_turn = Some(turn);
        self.emit(SessionEvent::TurnStarted { turn, origin });
    }

    fn finish_active_turn(&mut self) {
        let Some(turn) = self.state.get_mut().finish_turn() else {
            return;
        };
        self.announced_turn = None;
        if self.runtime().checkpoint_ready() {
            if let Err(error) = self.checkpoint() {
                self.emit_error(error.to_string());
            }
        } else {
            self.checkpoint_pending = true;
        }
        self.emit(SessionEvent::TurnEnded { turn });
    }

    fn finish_control_request(&mut self) {
        self.requested_control = None;
        for ack in std::mem::take(&mut self.control_acks) {
            let _ = ack.try_send(Ok(()));
        }
    }

    fn finish_close(&mut self) {
        let result = match self.checkpoint() {
            Ok(()) => Ok(()),
            Err(error) => {
                tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
                Err(SessionControlError::ClosePersistence)
            }
        };
        self.emit_closed();
        self.active_lease = None;
        self.close_requested = false;
        self.checkpoint_pending = false;
        self.requested_control = None;
        for ack in std::mem::take(&mut self.control_acks) {
            let _ = ack.try_send(Ok(()));
        }
        for ack in std::mem::take(&mut self.close_acks) {
            let _ = ack.try_send(result.clone());
        }
    }

    fn emit_closed(&mut self) {
        if self.events.is_some() {
            self.emit(SessionEvent::Closed);
            self.events = None;
        }
    }

    fn emit(&self, event: SessionEvent) {
        if let Some(events) = &self.events {
            events.emit(event);
        }
    }

    fn emit_error(&self, message: String) {
        self.emit(SessionEvent::Error { message });
    }

    fn checkpoint(&self) -> Result<(), super::persistence::SessionCheckpointError> {
        self.checkpointer
            .checkpoint(self.session, self.persistence, &self.state, self.runtime())
    }

    fn runtime(&self) -> &MultiagentRuntime<Filesystem, Http, Timer> {
        self.idle_runtime()
            .expect("session runtime is idle outside an actor drive")
    }

    fn idle_runtime(&self) -> Option<&MultiagentRuntime<Filesystem, Http, Timer>> {
        match self.execution.as_ref()? {
            RuntimeExecution::Idle(runtime) => Some(runtime),
            RuntimeExecution::Driving { .. } => None,
        }
    }

    fn take_runtime(&mut self) -> MultiagentRuntime<Filesystem, Http, Timer> {
        match self.execution.take() {
            Some(RuntimeExecution::Idle(runtime)) => runtime,
            Some(driving @ RuntimeExecution::Driving { .. }) => {
                self.execution = Some(driving);
                panic!("session runtime is already driving")
            }
            None => panic!("session runtime left in a transition state"),
        }
    }

    fn is_driving(&self) -> bool {
        matches!(self.execution, Some(RuntimeExecution::Driving { .. }))
    }

    fn driving_kind(&self) -> Option<RuntimeDriveKind> {
        match self.execution.as_ref()? {
            RuntimeExecution::Driving { kind, .. } => Some(*kind),
            RuntimeExecution::Idle(_) => None,
        }
    }

    fn driving_control(&self) -> Option<&DriveControl> {
        match self.execution.as_ref()? {
            RuntimeExecution::Driving { control, .. } => Some(control),
            RuntimeExecution::Idle(_) => None,
        }
    }

    fn start_user_input(&mut self, input: Message) {
        let runtime = self.take_runtime();
        let events = self.events.clone().unwrap_or_else(EventSink::disabled);
        let effort = self.state.get().reasoning_effort();
        let persistence = self.persistence;
        let api_manager = Arc::clone(&self.api_manager);
        let turn = self
            .state
            .get()
            .active_turn_id()
            .expect("user input belongs to an active turn");
        let control = DriveControl::new();
        let drive_control = control.clone();
        let future = Box::pin(
            async move {
                let mut runtime = runtime;
                let result = drive_user_input(
                    &mut runtime,
                    input,
                    effort,
                    persistence,
                    &events,
                    &drive_control,
                    &api_manager,
                )
                .await;
                RuntimeCompletion { runtime, result }
            }
            .instrument(tracing::info_span!("turn", run.turn = %turn, cause = "user_submit")),
        );
        self.execution = Some(RuntimeExecution::Driving {
            kind: RuntimeDriveKind::Foreground,
            control,
            future,
        });
    }

    fn start_pending_root_result(&mut self) {
        let runtime = self.take_runtime();
        let events = self.events.clone().unwrap_or_else(EventSink::disabled);
        let effort = self.state.get().reasoning_effort();
        let turn = self
            .state
            .get()
            .active_turn_id()
            .expect("a pending root result belongs to an active turn");
        let control = DriveControl::new();
        let drive_control = control.clone();
        let future = Box::pin(
            async move {
                let mut runtime = runtime;
                let result =
                    drive_pending_root_result(&mut runtime, effort, &events, &drive_control).await;
                RuntimeCompletion { runtime, result }
            }
            .instrument(tracing::info_span!("turn", run.turn = %turn, cause = "background_result")),
        );
        self.execution = Some(RuntimeExecution::Driving {
            kind: RuntimeDriveKind::Foreground,
            control,
            future,
        });
    }

    fn start_background(&mut self) {
        let runtime = self.take_runtime();
        let events = self.events.clone().unwrap_or_else(EventSink::disabled);
        let control = DriveControl::new();
        let drive_control = control.clone();
        let future = Box::pin(async move {
            let mut runtime = runtime;
            let result = drive_background(&mut runtime, &events, &drive_control).await;
            RuntimeCompletion { runtime, result }
        });
        self.execution = Some(RuntimeExecution::Driving {
            kind: RuntimeDriveKind::Background,
            control,
            future,
        });
    }

    fn start_stop(&mut self, mode: TurnStopMode) {
        let runtime = self.take_runtime();
        let future = Box::pin(async move {
            let mut runtime = runtime;
            runtime.stop_turn_tasks(mode).await;
            RuntimeCompletion {
                runtime,
                result: RuntimeDriveResult::Stopped,
            }
        });
        self.execution = Some(RuntimeExecution::Driving {
            kind: RuntimeDriveKind::Stop,
            control: DriveControl::new(),
            future,
        });
    }

    fn handle_runtime_finished(&mut self, kind: RuntimeDriveKind, result: RuntimeDriveResult) {
        if let RuntimeDriveResult::Driven(result) = result {
            let stop = self.emit_drive_result(result);
            if kind.is_foreground() && stop != DriveStop::Quiescent {
                self.finish_active_turn();
            }
        }
        if self.requested_control.is_some() || self.close_requested || self.delete_requested {
            self.finish_active_turn();
            self.finish_control_request();
        }
    }

    fn emit_drive_result(
        &self,
        result: Result<(DriveOutput, DriveStop), DeliverError>,
    ) -> DriveStop {
        match result {
            Ok((output, stop)) => {
                let mut emitted = false;
                for text in output.into_messages() {
                    self.emit(SessionEvent::Output(StreamPart::Delta(text)));
                    emitted = true;
                }
                if emitted {
                    self.emit(SessionEvent::Output(StreamPart::End));
                }
                stop
            }
            Err(error) => {
                let kind: &'static str = (&error).into();
                tracing::error!(name: "error", kind);
                self.emit_error(error.to_string());
                DriveStop::Quiescent
            }
        }
    }
}

enum ActorEvent {
    Command(Option<SessionCommand>),
    RuntimeFinished {
        kind: RuntimeDriveKind,
        result: RuntimeDriveResult,
    },
}

struct ActorPoll<'a, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    commands: Pin<&'a mut Receiver<SessionCommand>>,
    execution: &'a mut Option<RuntimeExecution<Filesystem, Http, Timer>>,
}

impl<Filesystem, Http, Timer> Future for ActorPoll<'_, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    type Output = ActorEvent;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        if let Poll::Ready(command) = this.commands.as_mut().poll_next(context) {
            return Poll::Ready(ActorEvent::Command(command));
        }
        let Some(RuntimeExecution::Driving { kind, future, .. }) = this.execution.as_mut() else {
            return Poll::Pending;
        };
        let kind = *kind;
        let Poll::Ready(completion) = future.as_mut().poll(context) else {
            return Poll::Pending;
        };
        *this.execution = Some(RuntimeExecution::Idle(completion.runtime));
        Poll::Ready(ActorEvent::RuntimeFinished {
            kind,
            result: completion.result,
        })
    }
}

async fn drive_user_input<Filesystem, Http, Timer>(
    runtime: &mut MultiagentRuntime<Filesystem, Http, Timer>,
    input: Message,
    effort: crate::config::ReasoningEffort,
    persistence: SessionPersistence,
    events: &EventSink,
    control: &DriveControl,
    api_manager: &Arc<RwLock<ClawApiManager>>,
) -> RuntimeDriveResult
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let result = if let Some(pending) = runtime.active_approval() {
        match runtime
            .set_root_context_block(effort.context_block())
            .map_err(DeliverError::from)
        {
            Ok(()) => {
                resolve_pending_approval(
                    runtime,
                    pending,
                    input.as_str(),
                    control,
                    events,
                    api_manager,
                )
                .await
            }
            Err(error) => Err(error),
        }
    } else {
        match runtime
            .deliver(input, effort.context_block(), persistence)
            .map_err(DeliverError::from)
        {
            Ok(()) => drive_root(runtime, control, events).await,
            Err(error) => Err(error),
        }
    };
    RuntimeDriveResult::Driven(result)
}

async fn drive_pending_root_result<Filesystem, Http, Timer>(
    runtime: &mut MultiagentRuntime<Filesystem, Http, Timer>,
    effort: crate::config::ReasoningEffort,
    events: &EventSink,
    control: &DriveControl,
) -> RuntimeDriveResult
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let result = if runtime.activate_pending_root_result() {
        runtime
            .set_root_context_block(effort.context_block())
            .map_err(DeliverError::from)
    } else {
        debug_assert!(false, "subagent turn requires one pending root result");
        Ok(())
    };
    let result = match result {
        Ok(()) => drive_root(runtime, control, events).await,
        Err(error) => Err(error),
    };
    RuntimeDriveResult::Driven(result)
}

async fn drive_background<Filesystem, Http, Timer>(
    runtime: &mut MultiagentRuntime<Filesystem, Http, Timer>,
    events: &EventSink,
    control: &DriveControl,
) -> RuntimeDriveResult
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let (output, stop) = runtime
        .drive_background_until_root_ready(control, events)
        .await;
    if let Some(mode) = stop_mode(stop) {
        runtime.stop_turn_tasks(mode).await;
    }
    RuntimeDriveResult::Driven(Ok((output, stop)))
}

async fn drive_root<Filesystem, Http, Timer>(
    runtime: &mut MultiagentRuntime<Filesystem, Http, Timer>,
    control: &DriveControl,
    events: &EventSink,
) -> Result<(DriveOutput, DriveStop), DeliverError>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let (output, stop) = runtime.drive_root_turn(control, events).await;
    if let Some(mode) = stop_mode(stop) {
        runtime.stop_turn_tasks(mode).await;
    }
    Ok((output, stop))
}

fn stop_mode(stop: DriveStop) -> Option<TurnStopMode> {
    match stop {
        DriveStop::Cancelled => Some(TurnStopMode::DeleteSpawnedAgents),
        DriveStop::Interrupted => Some(TurnStopMode::PreserveAgents),
        DriveStop::Quiescent | DriveStop::Woken => None,
    }
}

async fn resolve_pending_approval<Filesystem, Http, Timer>(
    runtime: &mut MultiagentRuntime<Filesystem, Http, Timer>,
    pending: PendingApproval,
    user_reply: &str,
    control: &DriveControl,
    events: &EventSink,
    api_manager: &Arc<RwLock<ClawApiManager>>,
) -> Result<(DriveOutput, DriveStop), DeliverError>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let resolution = match approval::resolve_permission_reply::<Http, Timer>(
        api_manager,
        &pending.summary,
        user_reply,
        control,
    )
    .await
    {
        Ok(resolution) => resolution,
        Err(ApprovalResolverError::Cancelled) => {
            runtime
                .stop_turn_tasks(TurnStopMode::DeleteSpawnedAgents)
                .await;
            return Ok((DriveOutput::default(), DriveStop::Cancelled));
        }
        Err(error) => return Err(error.into()),
    };

    let Some(decision) = resolution.clone().into_decision() else {
        let PermissionReplyResolution::Clarify(message) = resolution else {
            unreachable!("non-clarification resolutions map to approval decisions")
        };
        return Ok((DriveOutput::message(message), DriveStop::Quiescent));
    };
    runtime.resolve_active_approval(decision)?;
    drive_root(runtime, control, events).await
}
