use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{mpsc, Arc, RwLock};

use async_channel::{Receiver, Sender};
use claw_checkpoint::{FsCheckpointStorage, SharedCheckpointCoordinator};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawExecutor, ClawFs, ClawHttp, ClawTimer};
use claw_tool::ToolRegistry;
use futures_core::Stream;
use tracing::Instrument as _;

use crate::agent::FsAgentFactory;
use crate::config::ClawApiManager;
use crate::multiagent::AgentIdAllocator;
use crate::protocol::{EventSink, SessionId};
use crate::session::{
    load_session_restores, OpenSessionError, SessionActor, SessionActorExit, SessionCheckpointer,
    SessionCommand, SessionControlError, SessionEndpoint, SessionStore,
};

use super::checkpoint::load_agent_id_allocator;
use super::{OrchestratorBuildError, ORCHESTRATOR_TRACE_TASK, SYSTEM_TRACE_SCOPE};

/// Process-wide commands. Turn and control commands go directly to a session actor.
pub(super) enum Command {
    OpenSession {
        session: SessionId,
        events: EventSink,
        ack: Sender<Result<SessionEndpoint, OpenSessionError>>,
    },
    DeleteSession {
        session: SessionId,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Stop,
}

type ActorFuture = Pin<Box<dyn Future<Output = SessionActorExit>>>;

struct ActorTask {
    commands: Sender<SessionCommand>,
    future: ActorFuture,
}

struct Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
    sessions: Arc<SessionStore>,
    agent_ids: AgentIdAllocator,
    checkpointer: SessionCheckpointer<Filesystem>,
    api_manager: Arc<RwLock<ClawApiManager>>,
    dormant: HashMap<SessionId, SessionActor<Filesystem, Http, Timer>>,
    actors: HashMap<SessionId, ActorTask>,
}

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    #[allow(clippy::too_many_arguments)]
    fn new(
        tools: Arc<ToolRegistry>,
        checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
        persistence_dir: String,
        checkpoint_dir: String,
        skill_roots: Vec<String>,
        sessions: Arc<SessionStore>,
        api_manager: Arc<RwLock<ClawApiManager>>,
    ) -> Result<Self, OrchestratorBuildError> {
        let agent_ids = load_agent_id_allocator::<Filesystem>(&checkpoint_dir)?;
        let factory = Rc::new(FsAgentFactory::new(
            tools,
            persistence_dir,
            skill_roots,
            Arc::clone(&api_manager),
        )?);
        let checkpointer = SessionCheckpointer::new(checkpoints, agent_ids.clone());
        let restores = load_session_restores::<Filesystem>(&checkpoint_dir, sessions.as_ref())?;
        let mut dormant = HashMap::with_capacity(restores.len());
        for (session, restore) in restores {
            let span = tracing::info_span!("session.restore", run.session = %session);
            let persistence = sessions
                .persistence(session)
                .expect("only live session checkpoints are restored");
            let actor = span.in_scope(|| {
                SessionActor::restored(
                    session,
                    persistence,
                    Rc::clone(&factory),
                    agent_ids.clone(),
                    checkpointer.clone(),
                    Arc::clone(&api_manager),
                    restore,
                )
            })?;
            dormant.insert(session, actor);
        }
        Ok(Self {
            factory,
            sessions,
            agent_ids,
            checkpointer,
            api_manager,
            dormant,
            actors: HashMap::new(),
        })
    }

    async fn run(mut self, commands: Receiver<Command>) {
        let mut commands = Box::pin(commands);
        let mut stopping = false;
        loop {
            if stopping && self.actors.is_empty() {
                self.reject_queued_commands(commands.as_ref().get_ref());
                return;
            }
            match (EnginePoll {
                commands: (!stopping).then_some(commands.as_mut()),
                actors: &mut self.actors,
            })
            .await
            {
                EngineEvent::ActorExited(exit) => {
                    self.actors.remove(&exit.session());
                }
                EngineEvent::Command(Some(Command::OpenSession {
                    session,
                    events,
                    ack,
                })) => self.open_session(session, events, ack),
                EngineEvent::Command(Some(Command::DeleteSession { session, ack })) => {
                    self.delete_session(session, ack)
                }
                EngineEvent::Command(Some(Command::Stop)) | EngineEvent::Command(None) => {
                    stopping = true;
                    for actor in self.actors.values() {
                        let _ = actor.commands.try_send(SessionCommand::Shutdown);
                    }
                }
            }
        }
    }

    fn open_session(
        &mut self,
        session: SessionId,
        events: EventSink,
        ack: Sender<Result<SessionEndpoint, OpenSessionError>>,
    ) {
        let Some(persistence) = self.sessions.persistence(session) else {
            let _ = ack.try_send(Err(OpenSessionError::SessionNotFound(session)));
            return;
        };
        if !self.actors.contains_key(&session) {
            let actor = self.dormant.remove(&session).unwrap_or_else(|| {
                SessionActor::fresh(
                    session,
                    persistence,
                    Rc::clone(&self.factory),
                    self.agent_ids.clone(),
                    self.checkpointer.clone(),
                    Arc::clone(&self.api_manager),
                )
            });
            self.spawn_actor(session, actor);
        }
        let task = self
            .actors
            .get(&session)
            .expect("the session actor was just created");
        if task
            .commands
            .try_send(SessionCommand::Open {
                events,
                commands: task.commands.clone(),
                ack: ack.clone(),
            })
            .is_err()
        {
            let _ = ack.try_send(Err(OpenSessionError::WorkerStopped));
        }
    }

    fn delete_session(&mut self, session: SessionId, ack: Sender<Result<(), SessionControlError>>) {
        if !self.sessions.delete(session) {
            let _ = ack.try_send(Err(SessionControlError::SessionClosed(session)));
            return;
        }
        self.dormant.remove(&session);
        let Some(task) = self.actors.get(&session) else {
            let _ = ack.try_send(Ok(()));
            return;
        };
        if task
            .commands
            .try_send(SessionCommand::Delete { ack: ack.clone() })
            .is_err()
        {
            let _ = ack.try_send(Err(SessionControlError::WorkerStopped));
        }
    }

    fn spawn_actor(&mut self, session: SessionId, actor: SessionActor<Filesystem, Http, Timer>) {
        let (commands, receiver) = async_channel::unbounded();
        let future = Box::pin(actor.run(receiver).instrument(tracing::info_span!(
            "session",
            trace.task = %session,
            run.session = %session,
        )));
        let previous = self.actors.insert(session, ActorTask { commands, future });
        debug_assert!(previous.is_none());
    }

    fn reject_queued_commands(&self, commands: &Receiver<Command>) {
        while let Ok(command) = commands.try_recv() {
            match command {
                Command::OpenSession { ack, .. } => {
                    let _ = ack.try_send(Err(OpenSessionError::WorkerStopped));
                }
                Command::DeleteSession { ack, .. } => {
                    let _ = ack.try_send(Err(SessionControlError::WorkerStopped));
                }
                Command::Stop => {}
            }
        }
    }
}

enum EngineEvent {
    ActorExited(SessionActorExit),
    Command(Option<Command>),
}

struct EnginePoll<'a> {
    commands: Option<Pin<&'a mut Receiver<Command>>>,
    actors: &'a mut HashMap<SessionId, ActorTask>,
}

impl Future for EnginePoll<'_> {
    type Output = EngineEvent;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        if let Some(commands) = this.commands.as_mut() {
            if let Poll::Ready(command) = commands.as_mut().poll_next(context) {
                return Poll::Ready(EngineEvent::Command(command));
            }
        }
        for actor in this.actors.values_mut() {
            if let Poll::Ready(exit) = actor.future.as_mut().poll(context) {
                return Poll::Ready(EngineEvent::ActorExited(exit));
            }
        }
        Poll::Pending
    }
}

#[allow(clippy::too_many_arguments)]
pub(super) fn run_engine<Filesystem, Http, Timer, Executor>(
    tools: Arc<ToolRegistry>,
    checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
    persistence_dir: String,
    checkpoint_dir: String,
    skill_roots: Vec<String>,
    sessions: Arc<SessionStore>,
    command_rx: Receiver<Command>,
    ready: mpsc::Sender<Result<(), OrchestratorBuildError>>,
    api_manager: Arc<RwLock<ClawApiManager>>,
) where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
    Executor: ClawExecutor,
{
    let span = tracing::info_span!(
        "orchestrator",
        trace.task = ORCHESTRATOR_TRACE_TASK,
        run.system = SYSTEM_TRACE_SCOPE,
    );
    let engine = match span.in_scope(|| {
        Engine::<Filesystem, Http, Timer>::new(
            tools,
            checkpoints,
            persistence_dir,
            checkpoint_dir,
            skill_roots,
            sessions,
            api_manager,
        )
    }) {
        Ok(engine) => engine,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let _ = ready.send(Ok(()));
    Executor::block_on(engine.run(command_rx).instrument(span));
}
