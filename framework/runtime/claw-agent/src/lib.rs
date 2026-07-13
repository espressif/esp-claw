//! `claw_agent` wires tools and sessions to the core orchestrator.
//!
//! `AgentSystem` owns sessions and exposes session connections. Transport
//! routing, channel inbound/outbound conversion, and reply destinations live in
//! adapter crates above this layer.

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::{ClawApiConfig, InitError};
use claw_checkpoint::{
    BatchId, CheckpointCoordinatorInitError, CheckpointStorage, CheckpointStorageError,
    DurableBatchSnapshot, DurablePart, DurablePartError, DurablePartSnapshot, FsCheckpointStorage,
    LoadCheckpointError, SharedCheckpointCoordinator,
};
pub use claw_core::{
    ApiUsage, AttachmentId, AttachmentKind, AttachmentRecord, AttachmentRef, IterationId, Message,
    OpenSessionError, ReasoningEffort, SessionControl, SessionControlError, SessionEvent,
    SessionEventStream, SessionId, SessionPersistence, TurnId,
};
use claw_core::{Orchestrator, OrchestratorBuildError};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, FsError};
#[cfg(feature = "host-backends")]
use claw_interface::{DiskFs, RealHttp, TokioTimer};
use claw_tool::{ToolRegistry, ToolRegistryError};

#[cfg(feature = "host-backends")]
pub type HostAgentSystem = AgentSystem<DiskFs, RealHttp, TokioTimer>;

pub type AgentResult<T> = Result<T, AgentError>;

const CHECKPOINT_DIR: &str = "checkpoint";
const TOOL_REGISTRY_BATCH: &str = "tool-registry";
const TOOL_REGISTRY_BATCH_ID: BatchId = BatchId::new(1);
const TOOL_REGISTRY_PART: &str = "tool-registry";
const CHECKPOINT_INTERVAL: u64 = 30;
const CHECKPOINT_HISTORY: u64 = 2;

/// Explicit storage root for an [`AgentSystem`], plus the skill roots the agent
/// factory scans to populate every agent's skill catalog.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentPersistenceConfig {
    pub persistence_root: String,
    /// Skill roots in priority order (e.g. DATA before SYSTEM). Empty means no
    /// filesystem skills are loaded.
    pub skill_roots: Vec<String>,
}

/// What can go wrong while building or driving an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// An LLM API config could not be linked because a required field is empty.
    #[error(transparent)]
    LlmConfig(#[from] InitError),
    /// Building the core orchestrator failed.
    #[error(transparent)]
    Orchestrator(#[from] OrchestratorBuildError),
    /// The tool registry failed.
    #[error(transparent)]
    Tool(#[from] ToolRegistryError),
    /// Opening a session event stream failed.
    #[error(transparent)]
    OpenSession(#[from] OpenSessionError),
    /// The scratch storage root could not be cleared before startup.
    #[error("failed to clear agent storage at {path}: {source}")]
    StorageClear {
        path: String,
        #[source]
        source: FsError,
    },
    /// Checkpoint storage metadata could not be read or written.
    #[error("checkpoint storage failed: {0}")]
    CheckpointStorage(#[from] CheckpointStorageError),
    /// The shared checkpoint coordinator could not be initialized.
    #[error("checkpoint storage failed: coordinator initialization failed: {0}")]
    CheckpointCoordinatorInit(#[from] CheckpointCoordinatorInitError),
    /// A checkpoint exists but cannot be loaded.
    #[error("checkpoint load failed: {0}")]
    CheckpointLoad(#[from] LoadCheckpointError),
    /// A checkpoint part could not be exported or restored.
    #[error("checkpoint durable part failed: {0}")]
    CheckpointPart(#[from] DurablePartError),
    /// A checkpoint exists but does not contain the expected durable part.
    #[error("checkpoint is missing part {part} in batch {batch}")]
    MissingCheckpointPart {
        batch: &'static str,
        part: &'static str,
    },
}

/// A ready-to-drive agent runtime.
///
/// The `Filesystem`/`Http`/`Timer` backends select which concrete filesystem,
/// HTTP, and timer the orchestrator's drive worker uses; they are only needed at
/// construction, so they are held as a marker (the built [`Orchestrator`] handle
/// is backend-erased and `Send + Sync`).
type BackendMarker<Filesystem, Http, Timer> = PhantomData<fn() -> (Filesystem, Http, Timer)>;

pub struct AgentSystem<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    tools: Arc<ToolRegistry>,
    orchestrator: Orchestrator,
    _marker: BackendMarker<Filesystem, Http, Timer>,
}

impl<Filesystem, Http, Timer> AgentSystem<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build a fully injectable agent system, spawning the orchestrator's drive
    /// worker via the [`ClawThread`] policy `Thread` (`StdThread` on host,
    /// `EspIdfThread` on device) and driving its `!Send` engine with the injected
    /// [`ClawExecutor`] `Executor` (`TokioExecutor` on host,
    /// `EspIdfExecutor` on device).
    /// Both are zero-sized policies selected purely by type parameter, like the
    /// `Filesystem`/`Http`/`Timer` backends.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when storage cleanup or orchestrator construction fails.
    pub fn new<Thread, Executor>(persistence: AgentPersistenceConfig) -> AgentResult<Self>
    where
        Thread: ClawThread,
        Executor: ClawExecutor + 'static,
    {
        let persistence_root = persistence.persistence_root.trim_end_matches('/');
        let checkpoint_root = if persistence.persistence_root == "/" {
            format!("/{CHECKPOINT_DIR}")
        } else if persistence_root.is_empty() {
            CHECKPOINT_DIR.to_owned()
        } else {
            format!("{persistence_root}/{CHECKPOINT_DIR}")
        };
        let checkpoints = SharedCheckpointCoordinator::new(
            FsCheckpointStorage::<Filesystem>::new(checkpoint_root.clone()),
            CHECKPOINT_INTERVAL,
            CHECKPOINT_HISTORY,
        )?;
        let tools = Arc::new(load_tool_registry::<Filesystem>(&checkpoint_root)?);
        let tool_checkpoints = checkpoints.clone();
        tools.set_checkpoint_hook(move |generation, state, hint| {
            tool_checkpoints.checkpoint(vec![DurableBatchSnapshot::new(
                TOOL_REGISTRY_BATCH,
                TOOL_REGISTRY_BATCH_ID,
                vec![DurablePartSnapshot::new(
                    TOOL_REGISTRY_PART,
                    generation,
                    state,
                    hint,
                )],
            )])?;
            Ok(())
        });
        let orchestrator = Orchestrator::new_with_checkpoint_coordinator::<
            Filesystem,
            Http,
            Timer,
            Thread,
            Executor,
        >(
            Arc::clone(&tools),
            checkpoints,
            persistence.persistence_root,
            persistence.skill_roots,
        )?;

        Ok(Self {
            tools,
            orchestrator,
            _marker: PhantomData,
        })
    }

    /// Tool registry used by this system.
    pub fn tool_registry(&self) -> &ToolRegistry {
        &self.tools
    }

    /// Start every registered tool.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when the tool registry fails to start.
    pub fn start_all(&self) -> AgentResult<()> {
        self.tools.start_all()?;
        Ok(())
    }

    /// Stop every registered tool.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when the tool registry fails to stop.
    pub fn stop_all(&self) -> AgentResult<()> {
        self.tools.stop_all()?;
        Ok(())
    }

    /// Open a live session's command and event halves.
    ///
    /// The returned [`SessionControl`] accepts user inputs and session control
    /// commands; the returned [`SessionEventStream`] is the only user-visible
    /// event outlet for the session.
    ///
    /// # Errors
    ///
    /// Returns [`OpenSessionError`] when the session is missing, already open, or
    /// the orchestrator worker is stopped.
    pub fn open_session(
        &self,
        session: SessionId,
    ) -> AgentResult<(SessionControl, SessionEventStream)> {
        Ok(self.orchestrator.open_session(session)?)
    }

    /// Register an LLM API config for a usage (root/subagent/memory/compaction).
    ///
    /// De-duplicated by model; when `default` is set it becomes the fallback for
    /// usages without an explicit binding. Updates take effect at the start of the
    /// next turn (see `ClawApiManager`), so this never disturbs an in-flight turn.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError::LlmConfig`] without changing bindings when `api` is
    /// invalid.
    pub fn link_api(&self, api: ClawApiConfig, usage: ApiUsage, default: bool) -> AgentResult<()> {
        self.orchestrator.link_api(api, usage, default)?;
        Ok(())
    }

    /// Create a fresh isolated conversation session with explicit persistence.
    /// Ephemeral sessions keep their transcript only for this process.
    pub fn new_session(&self, persistence: SessionPersistence) -> SessionId {
        self.orchestrator.session_create(persistence)
    }

    /// Return the live conversation sessions.
    pub fn list_sessions(&self) -> Vec<SessionId> {
        self.orchestrator.session_list()
    }

    /// Delete a live conversation session.
    ///
    /// If the session is currently open, its event stream receives
    /// [`SessionEvent::Closed`].
    ///
    /// # Errors
    ///
    /// Returns [`SessionControlError`] when the session is already gone or the
    /// orchestrator worker is stopped.
    pub fn delete_session(&self, session: SessionId) -> Result<(), SessionControlError> {
        self.orchestrator.session_delete(session)
    }
}

fn load_tool_registry<Filesystem: ClawFs>(checkpoint_root: &str) -> AgentResult<ToolRegistry> {
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_root.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(ToolRegistry::new());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut saw_batch = false;
    for batch in checkpoint.batches {
        if batch.name != TOOL_REGISTRY_BATCH || batch.id != TOOL_REGISTRY_BATCH_ID {
            continue;
        }
        saw_batch = true;
        for part in batch.parts {
            if part.name == TOOL_REGISTRY_PART {
                return Ok(<ToolRegistry as DurablePart>::restore_from_state(
                    part.state.as_slice(),
                )?);
            }
        }
    }
    if saw_batch {
        Err(AgentError::MissingCheckpointPart {
            batch: TOOL_REGISTRY_BATCH,
            part: TOOL_REGISTRY_PART,
        })
    } else {
        Ok(ToolRegistry::new())
    }
}
