mod command_loop;
mod drive_loop;
mod instance_store;
mod persistence;
mod session_drive;
mod session_io;
mod turn;

use core::cell::RefCell;
use core::future::Future;
use core::pin::Pin;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{mpsc, Arc, RwLock};

use async_channel::Receiver;
use claw_checkpoint::{DurableState, FsCheckpointStorage, SharedCheckpointCoordinator};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawExecutor, ClawFs, ClawHttp, ClawTimer};
use claw_tool::ToolRegistry;

use crate::agent::FsAgentFactory;
use crate::config::ClawApiManager;
use crate::session::{SessionId, SessionStore};

use super::checkpoint::{load_engine_state, load_session_drives, load_session_instances};
use super::instance::OrchestratorInstance;
use super::OrchestratorBuildError;

pub(crate) use self::instance_store::InstanceWork;
pub(super) use self::persistence::EngineState;
pub(super) use self::session_drive::{SessionDrive, SessionDriveState};
pub(super) use self::session_io::Command;

pub(super) type DriveFuture = Pin<Box<dyn Future<Output = ()>>>;

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
    let engine_state = match load_engine_state::<Filesystem>(&checkpoint_dir) {
        Ok(state) => state,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let engine = match Engine::<Filesystem, Http, Timer>::new(
        tools,
        checkpoints,
        persistence_dir,
        checkpoint_dir,
        skill_roots,
        sessions,
        engine_state,
        api_manager,
    ) {
        Ok(engine) => Rc::new(engine),
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let _ = ready.send(Ok(()));
    Executor::block_on(engine.run(command_rx));
}

pub(super) struct Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
    pub(super) checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
    pub(super) instances:
        RefCell<HashMap<SessionId, OrchestratorInstance<Filesystem, Http, Timer>>>,
    pub(super) drives: RefCell<HashMap<SessionId, SessionDrive>>,
    sessions: Arc<SessionStore>,
    pub(super) state: DurableState<EngineState>,
    /// Per-usage LLM config, shared with the orchestrator handle. Read at the
    /// start of each turn to pick the config for that turn.
    pub(super) api_manager: Arc<RwLock<ClawApiManager>>,
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
        state: EngineState,
        api_manager: Arc<RwLock<ClawApiManager>>,
    ) -> Result<Self, OrchestratorBuildError> {
        let factory = Arc::new(FsAgentFactory::<Filesystem, Http, Timer>::new(
            tools,
            persistence_dir,
            skill_roots,
            Arc::clone(&api_manager),
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
            checkpoints,
            instances: RefCell::new(instances),
            drives: RefCell::new(drives),
            sessions,
            state: DurableState::new(state),
            api_manager,
        })
    }
}
