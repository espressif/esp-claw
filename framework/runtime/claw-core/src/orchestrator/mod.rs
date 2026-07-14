//! Process-wide facade and worker for session actors.
//!
//! The worker owns session lookup and cooperatively polls one actor per live
//! session. Turn state, agent graphs, and model-facing tools do not live here.

mod checkpoint;
mod engine;
mod handle;

use std::error::Error as StdError;
use std::io;

use claw_checkpoint::{BatchId, CheckpointStorageError, DurablePartError, LoadCheckpointError};
use claw_memory::LongTermInitError;
use claw_skill::SkillError;

use crate::agent::FsAgentFactoryError;
use crate::session::SessionRestoreLoadError;

pub use handle::Orchestrator;

/// What can go wrong while building an [`Orchestrator`].
#[derive(Debug, thiserror::Error)]
pub enum OrchestratorBuildError {
    #[error("persistence directory is required")]
    MissingPersistenceDir,
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(#[from] LongTermInitError),
    #[error("failed to load skill catalog: {0}")]
    SkillRegistry(#[from] SkillError),
    #[error("failed to read checkpoint storage: {0}")]
    CheckpointStorage(#[from] CheckpointStorageError),
    #[error("failed to load checkpoint: {0}")]
    CheckpointLoad(#[from] LoadCheckpointError),
    #[error("failed to restore checkpoint part: {0}")]
    CheckpointRestore(#[from] DurablePartError),
    #[error("failed to restore checkpointed multiagent runtime: {source}")]
    CheckpointMultiagentRestore {
        #[source]
        source: Box<dyn StdError + Send + Sync>,
    },
    #[error("checkpoint is missing part {part} in batch {batch}")]
    MissingCheckpointPart {
        batch: &'static str,
        part: &'static str,
    },
    #[error("failed to spawn the orchestrator worker: {0}")]
    WorkerSpawn(#[from] io::Error),
    #[error("orchestrator worker exited before signalling readiness")]
    WorkerExitedBeforeReady,
}

impl From<FsAgentFactoryError> for OrchestratorBuildError {
    fn from(error: FsAgentFactoryError) -> Self {
        match error {
            FsAgentFactoryError::MissingPersistenceDir => Self::MissingPersistenceDir,
            FsAgentFactoryError::LongTermInit(source) => Self::LongTermInit(source),
            FsAgentFactoryError::SkillRegistry(source) => Self::SkillRegistry(source),
        }
    }
}

impl From<crate::multiagent::MultiagentRestoreError> for OrchestratorBuildError {
    fn from(source: crate::multiagent::MultiagentRestoreError) -> Self {
        Self::CheckpointMultiagentRestore {
            source: Box::new(source),
        }
    }
}

impl From<SessionRestoreLoadError> for OrchestratorBuildError {
    fn from(error: SessionRestoreLoadError) -> Self {
        match error {
            SessionRestoreLoadError::Storage(source) => Self::CheckpointStorage(source),
            SessionRestoreLoadError::Load(source) => Self::CheckpointLoad(source),
            SessionRestoreLoadError::Restore(source) => Self::CheckpointRestore(source),
            SessionRestoreLoadError::MissingPart { batch, part } => {
                Self::MissingCheckpointPart { batch, part }
            }
        }
    }
}

const ENGINE_WORKER_STACK_SIZE: usize = 64 * 1024;
const CHECKPOINT_DIR: &str = "checkpoint";
const SYSTEM_TRACE_SCOPE: &str = "agent-system";
const ORCHESTRATOR_TRACE_TASK: &str = "orchestrator";
const SESSION_REGISTRY_BATCH: &str = "session-registry";
const SESSION_REGISTRY_BATCH_ID: BatchId = BatchId::new(1);
const SESSION_STORE_PART: &str = "session-store";
