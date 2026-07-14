use std::collections::HashMap;

use claw_checkpoint::{
    BatchId, ChangePatternHint, CheckpointError, CheckpointStorage, CheckpointStorageError,
    DurableBatchSnapshot, DurablePartError, DurablePartSnapshot, DurableState, DurableStateCodec,
    FsCheckpointStorage, LoadCheckpointError, SharedCheckpointCoordinator, StorageHint,
    StorageSizeHint,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::multiagent::{AgentIdAllocator, MultiagentRestore, MultiagentRuntime};
use crate::protocol::{SessionId, SessionPersistence};

use super::registry::SessionStore;
use super::state::SessionState;

pub(crate) const ORCHESTRATOR_BATCH: &str = "orchestrator";
pub(crate) const ORCHESTRATOR_BATCH_ID: BatchId = BatchId::new(1);
pub(crate) const AGENT_ID_ALLOCATOR_PART: &str = "agent-id-allocator";
pub(crate) const SESSION_RUNTIME_BATCH: &str = "session-runtime";
pub(crate) const SESSION_STATE_PART: &str = "session-state";
pub(crate) const MULTIAGENT_RUNTIME_PART: &str = "multiagent-runtime";

pub(crate) struct SessionRestore {
    pub(super) state: SessionState,
    pub(super) multiagent: MultiagentRestore,
}

#[derive(Debug, thiserror::Error)]
pub(crate) enum SessionRestoreLoadError {
    #[error("failed to read checkpoint storage: {0}")]
    Storage(#[from] CheckpointStorageError),
    #[error("failed to load checkpoint: {0}")]
    Load(#[from] LoadCheckpointError),
    #[error("failed to restore checkpoint part: {0}")]
    Restore(#[from] DurablePartError),
    #[error("checkpoint is missing part {part} in batch {batch}")]
    MissingPart {
        batch: &'static str,
        part: &'static str,
    },
}

pub(crate) fn load_session_restores<Filesystem: ClawFs>(
    checkpoint_dir: &str,
    sessions: &SessionStore,
) -> Result<HashMap<SessionId, SessionRestore>, SessionRestoreLoadError> {
    let storage = FsCheckpointStorage::<Filesystem>::new(checkpoint_dir.to_owned());
    let Some(step) = storage.latest_step()? else {
        return Ok(HashMap::new());
    };
    let checkpoint = storage.load_checkpoint(step)?;
    let mut parts = HashMap::new();
    for batch in checkpoint.batches {
        if batch.name != SESSION_RUNTIME_BATCH {
            continue;
        }
        let session = SessionId::new(batch.id.0);
        if !sessions.contains(session) {
            continue;
        }
        let entry = parts.entry(session).or_insert_with(|| (None, None));
        for part in batch.parts {
            match part.name.as_str() {
                SESSION_STATE_PART => entry.0 = Some(part.state),
                MULTIAGENT_RUNTIME_PART => entry.1 = Some(part.state),
                _ => {}
            }
        }
    }

    let mut restores = HashMap::with_capacity(parts.len());
    for (session, (state, multiagent)) in parts {
        let state = state.ok_or(SessionRestoreLoadError::MissingPart {
            batch: SESSION_RUNTIME_BATCH,
            part: SESSION_STATE_PART,
        })?;
        let multiagent = multiagent.ok_or(SessionRestoreLoadError::MissingPart {
            batch: SESSION_RUNTIME_BATCH,
            part: MULTIAGENT_RUNTIME_PART,
        })?;
        restores.insert(
            session,
            SessionRestore {
                state: SessionState::decode_state(state.as_slice())?,
                multiagent: MultiagentRestore::decode_state(multiagent.as_slice())?,
            },
        );
    }
    Ok(restores)
}

#[derive(Debug, thiserror::Error)]
pub(crate) enum SessionCheckpointError {
    #[error(transparent)]
    Checkpoint(#[from] CheckpointError),
    #[error(transparent)]
    Export(#[from] DurablePartError),
}

pub(crate) struct SessionCheckpointer<Filesystem: ClawFs> {
    checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
    agent_ids: AgentIdAllocator,
}

impl<Filesystem: ClawFs> Clone for SessionCheckpointer<Filesystem> {
    fn clone(&self) -> Self {
        Self {
            checkpoints: self.checkpoints.clone(),
            agent_ids: self.agent_ids.clone(),
        }
    }
}

impl<Filesystem: ClawFs> SessionCheckpointer<Filesystem> {
    pub(crate) fn new(
        checkpoints: SharedCheckpointCoordinator<FsCheckpointStorage<Filesystem>>,
        agent_ids: AgentIdAllocator,
    ) -> Self {
        Self {
            checkpoints,
            agent_ids,
        }
    }

    pub(super) fn checkpoint<Http, Timer>(
        &self,
        session: SessionId,
        persistence: SessionPersistence,
        state: &DurableState<SessionState>,
        multiagent: &MultiagentRuntime<Filesystem, Http, Timer>,
    ) -> Result<(), SessionCheckpointError>
    where
        Filesystem: 'static,
        Http: ClawHttp + StreamingHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    {
        if persistence != SessionPersistence::Persistent {
            return Ok(());
        }
        let state_blob = state.export_state()?.into_owned();
        let state_snapshot = DurablePartSnapshot::new(
            SESSION_STATE_PART,
            state.generation(),
            state_blob,
            StorageHint {
                size: StorageSizeHint::Small,
                change: ChangePatternHint::Arbitrary,
            },
        );
        let multiagent_snapshot = DurablePartSnapshot::capture(multiagent)?;
        let allocator_snapshot = DurablePartSnapshot::capture(&self.agent_ids)?;
        self.checkpoints.checkpoint_now(vec![
            DurableBatchSnapshot::new(
                ORCHESTRATOR_BATCH,
                ORCHESTRATOR_BATCH_ID,
                vec![allocator_snapshot],
            ),
            DurableBatchSnapshot::new(
                SESSION_RUNTIME_BATCH,
                BatchId::new(session.0),
                vec![state_snapshot, multiagent_snapshot],
            ),
        ])?;
        Ok(())
    }
}
