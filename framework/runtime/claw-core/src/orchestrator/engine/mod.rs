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
use std::sync::{mpsc, Arc};

use async_channel::Receiver;
use claw_api::ClawApiConfig;
use claw_checkpoint::{DurableState, FsCheckpointStorage, SharedCheckpointCoordinator};
use claw_interface::{ClawExecutor, ClawFs, ClawHttp, ClawTimer};
use claw_tool::ToolRegistry;

use crate::agent::FsAgentFactory;
use crate::session::{SessionId, SessionStore};

use super::checkpoint::{load_engine_state, load_session_drives, load_session_instances};
use super::instance::OrchestratorInstance;
use super::OrchestratorBuildError;

pub(crate) use self::instance_store::InstanceWork;
pub(super) use self::persistence::EngineState;
pub(super) use self::session_drive::{SessionDrive, SessionDriveState};
pub(super) use self::session_io::Command;

pub(super) type DriveFuture = Pin<Box<dyn Future<Output = ()>>>;

pub(super) fn run_engine<Filesystem, Http, Timer, Executor>(
    tools: Arc<ToolRegistry>,
    checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
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
        checkpoints,
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
    Executor::block_on(engine.run(command_rx));
}

pub(super) struct Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
    approval_llm_config: ClawApiConfig,
    pub(super) checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
    pub(super) instances:
        RefCell<HashMap<SessionId, OrchestratorInstance<Filesystem, Http, Timer>>>,
    pub(super) drives: RefCell<HashMap<SessionId, SessionDrive>>,
    sessions: Arc<SessionStore>,
    pub(super) state: DurableState<EngineState>,
}

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn new(
        tools: Arc<ToolRegistry>,
        checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
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
            checkpoints,
            instances: RefCell::new(instances),
            drives: RefCell::new(drives),
            sessions,
            state: DurableState::new(state),
        })
    }
}
