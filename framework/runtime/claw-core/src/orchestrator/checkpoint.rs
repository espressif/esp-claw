use std::collections::HashMap;
use std::rc::Rc;

use claw_checkpoint::{
    BatchId, CheckpointError, CheckpointStorage, DurableBatchSnapshot, DurablePartError,
    DurableStateCodec, FsCheckpointStorage, PartStateBlob, SharedCheckpointCoordinator,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentIdAllocator, FsAgentFactory};
use crate::session::{SessionId, SessionStore, SessionStoreState};

use super::engine::{EngineState, SessionDriveState, SessionRuntime};
use super::instance::{OrchestratorInstance, OrchestratorInstanceRestore};
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

pub(super) fn load_session_runtimes<Filesystem, Http, Timer>(
    checkpoint_dir: &str,
    sessions: &SessionStore,
    factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
    agent_id_allocator: AgentIdAllocator,
) -> Result<HashMap<SessionId, SessionRuntime<Filesystem, Http, Timer>>, OrchestratorBuildError>
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
    let mut runtime_parts = HashMap::<
        SessionId,
        (
            Option<PartStateBlob<'static>>,
            Option<PartStateBlob<'static>>,
        ),
    >::new();
    for batch in checkpoint.batches {
        if batch.name != SESSION_RUNTIME_BATCH {
            continue;
        }
        let session = SessionId::new(batch.id.0);
        if !sessions.contains(session) {
            continue;
        }
        let entry = runtime_parts.entry(session).or_default();
        for part in batch.parts {
            match part.name.as_str() {
                SESSION_DRIVE_PART => {
                    entry.0 = Some(part.state);
                }
                ORCHESTRATOR_INSTANCE_PART => {
                    entry.1 = Some(part.state);
                }
                _ => {}
            }
        }
    }

    // The drive is the required owner of a session runtime. Validate every
    // drive before decoding optional instance state so a missing drive cannot
    // be masked by corruption in a part that has no runtime to attach to.
    let mut decoded_drives = Vec::with_capacity(runtime_parts.len());
    for (session, (drive_state, instance_state)) in runtime_parts {
        let Some(drive_state) = drive_state else {
            return Err(OrchestratorBuildError::MissingCheckpointPart {
                batch: SESSION_RUNTIME_BATCH,
                part: SESSION_DRIVE_PART,
            });
        };
        decoded_drives.push((
            session,
            SessionDriveState::decode_state(drive_state.as_slice())?,
            instance_state,
        ));
    }

    let mut runtimes = HashMap::with_capacity(decoded_drives.len());
    for (session, drive_state, instance_state) in decoded_drives {
        let instance = instance_state
            .map(|state| -> Result<_, OrchestratorBuildError> {
                let state = OrchestratorInstanceRestore::decode_state(state.as_slice())?;
                Ok(OrchestratorInstance::from_restored_state(
                    session,
                    Rc::clone(&factory),
                    agent_id_allocator.clone(),
                    state,
                )?)
            })
            .transpose()?;
        runtimes.insert(
            session,
            SessionRuntime::from_restored_parts(drive_state, instance),
        );
    }
    Ok(runtimes)
}
