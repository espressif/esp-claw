//! Layer 1 orchestrator: a `Send` handle over a `!Send` drive engine that runs
//! on one owned worker thread.
//!
//! [`Orchestrator`] is a cheap, `Send + Sync` handle: it validates sessions
//! synchronously (via a shared [`SessionStore`]) and forwards work to the engine
//! as [`Command`]s over an async channel. The engine ([`Engine`]) is `!Send` (it
//! owns live agent graphs), so it is built *inside* the worker thread from
//! `Send` config and `block_on`-driven there. The engine multiplexes every live
//! session's drive on that single thread — cooperative concurrency, not
//! parallelism — the same way the async HTTP seam yields between EAGAIN steps.
//!
//! Channel routing is owned by the layer above this crate.

mod approval;
mod checkpoint;
mod control;
mod engine;
mod handle;
mod instance;
mod reasoning;

use core::pin::Pin;
use std::error::Error as StdError;
use std::io;

use async_channel::{Receiver, Sender};
use claw_checkpoint::{BatchId, CheckpointStorageError, DurablePartError, LoadCheckpointError};
use claw_memory::LongTermInitError;
use claw_skill::SkillError;

use crate::agent::FsAgentFactoryError;
use crate::event::SessionEvent;
use crate::session::SessionId;

pub use self::handle::Orchestrator;
pub use self::reasoning::ReasoningEffort;

/// What can go wrong while building an [`Orchestrator`].
#[derive(Debug, thiserror::Error)]
pub enum OrchestratorBuildError {
    /// No persistence directory was provided.
    #[error("persistence directory is required")]
    MissingPersistenceDir,
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
    #[error("failed to restore checkpointed session instance: {source}")]
    CheckpointInstanceRestore {
        #[source]
        source: Box<dyn StdError + Send + Sync>,
    },
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
            FsAgentFactoryError::LongTermInit(source) => Self::LongTermInit(source),
            FsAgentFactoryError::SkillRegistry(source) => Self::SkillRegistry(source),
        }
    }
}

impl From<instance::OrchestratorInstanceRestoreError> for OrchestratorBuildError {
    fn from(source: instance::OrchestratorInstanceRestoreError) -> Self {
        Self::CheckpointInstanceRestore {
            source: Box::new(source),
        }
    }
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
    /// The session closed, but its final state could not be checkpointed.
    #[error("failed to persist closed session state")]
    ClosePersistence,
}

/// Cloneable write/control half of an open session.
#[derive(Clone)]
pub struct SessionControl {
    session: SessionId,
    command_tx: Sender<engine::Command>,
}

/// The read/event half of an open session.
pub struct SessionEventStream {
    events: Pin<Box<Receiver<SessionEvent>>>,
}

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
