use claw_checkpoint::{
    BatchId, CheckpointError, CheckpointStorage, DurableBatchSnapshot, DurablePart,
    DurablePartError, DurableStateCodec, FsCheckpointStorage, SharedCheckpointCoordinator,
};
use claw_interface::ClawFs;

use crate::multiagent::AgentIdAllocator;
use crate::protocol::SessionId;
use crate::session::{
    SessionStore, SessionStoreState, AGENT_ID_ALLOCATOR_PART, ORCHESTRATOR_BATCH,
    ORCHESTRATOR_BATCH_ID, SESSION_RUNTIME_BATCH,
};

use super::{
    OrchestratorBuildError, SESSION_REGISTRY_BATCH, SESSION_REGISTRY_BATCH_ID, SESSION_STORE_PART,
};

#[derive(Debug, thiserror::Error)]
pub(super) enum SessionRegistryCheckpointError {
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

pub(super) fn load_agent_id_allocator<Filesystem: ClawFs>(
    checkpoint_dir: &str,
) -> Result<AgentIdAllocator, OrchestratorBuildError> {
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_dir.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(AgentIdAllocator::new());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut saw_batch = false;
    for batch in checkpoint.batches {
        if batch.name != ORCHESTRATOR_BATCH || batch.id != ORCHESTRATOR_BATCH_ID {
            continue;
        }
        saw_batch = true;
        for part in batch.parts {
            if part.name == AGENT_ID_ALLOCATOR_PART {
                return Ok(AgentIdAllocator::restore_from_state(part.state.as_slice())?);
            }
        }
    }
    if saw_batch {
        Err(OrchestratorBuildError::MissingCheckpointPart {
            batch: ORCHESTRATOR_BATCH,
            part: AGENT_ID_ALLOCATOR_PART,
        })
    } else {
        Ok(AgentIdAllocator::new())
    }
}
