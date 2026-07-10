use std::collections::HashMap;
use std::sync::Arc;

use claw_checkpoint::{
    BatchId, CheckpointError, CheckpointStorage, DurableBatchSnapshot, DurablePartError,
    DurableStateCodec, FsCheckpointStorage, SharedCheckpointCoordinator,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentIdAllocator, FsAgentFactory};
use crate::session::{SessionId, SessionStore, SessionStoreState};

use super::engine::{EngineState, SessionDrive, SessionDriveState};
use super::instance::{OrchestratorInstance, OrchestratorInstanceState};
use super::{
    OrchestratorBuildError, ENGINE_BATCH, ENGINE_BATCH_ID, ENGINE_PART, ORCHESTRATOR_INSTANCE_PART,
    SESSION_DRIVE_PART, SESSION_REGISTRY_BATCH, SESSION_REGISTRY_BATCH_ID, SESSION_RUNTIME_BATCH,
    SESSION_STORE_PART,
};

#[derive(Debug, thiserror::Error)]
pub(super) enum SessionRegistryCheckpointError {
    #[error(transparent)]
    Checkpoint(#[from] CheckpointError),
    #[error(transparent)]
    Export(#[from] DurablePartError),
}

#[derive(Debug, thiserror::Error)]
pub(super) enum RuntimeCheckpointError {
    #[error(transparent)]
    Checkpoint(#[from] CheckpointError),
    #[error(transparent)]
    Export(#[from] DurablePartError),
}

pub(super) fn checkpoint_session_registry<Filesystem: ClawFs>(
    checkpoints: &SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
    sessions: &SessionStore,
    removed_sessions: &[SessionId],
) -> Result<(), SessionRegistryCheckpointError> {
    let removed_batches = removed_sessions
        .iter()
        .map(|session| (SESSION_RUNTIME_BATCH, BatchId::new(session.0)))
        .collect();
    sessions.with_durable_snapshot(|snapshot| {
        checkpoints.checkpoint_and_remove(
            vec![DurableBatchSnapshot::new(
                SESSION_REGISTRY_BATCH,
                SESSION_REGISTRY_BATCH_ID,
                vec![snapshot],
            )],
            removed_batches,
        )
    })??;
    Ok(())
}

pub(super) fn load_session_store_state<Filesystem: ClawFs>(
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

pub(super) fn load_engine_state<Filesystem: ClawFs>(
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

pub(super) fn load_session_drives<Filesystem: ClawFs>(
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

pub(super) fn load_session_instances<Filesystem, Http, Timer>(
    checkpoint_dir: &str,
    sessions: &SessionStore,
    factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
    agent_id_allocator: AgentIdAllocator,
) -> Result<HashMap<SessionId, OrchestratorInstance<Filesystem, Http, Timer>>, OrchestratorBuildError>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
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
