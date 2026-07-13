use claw_checkpoint::{
    BatchId, ChangePatternHint, DurableBatchSnapshot, DurablePart, DurablePartError,
    DurablePartSnapshot, DurableStateCodec, PartGeneration, PartStateBlob, PartStateSlice,
    StorageHint, StorageSizeHint,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use serde::{Deserialize, Serialize};

use crate::agent::AgentIdAllocator;
use crate::session::SessionId;

use super::super::checkpoint::RuntimeCheckpointError;
use super::super::{ENGINE_BATCH, ENGINE_BATCH_ID, SESSION_RUNTIME_BATCH};
use super::Engine;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(in crate::orchestrator) struct EngineState {
    pub(super) agent_id_allocator: AgentIdAllocator,
}

impl Default for EngineState {
    fn default() -> Self {
        Self {
            agent_id_allocator: AgentIdAllocator::new(),
        }
    }
}

impl DurableStateCodec for EngineState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }
}

impl<Filesystem, Http, Timer> DurablePart for Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn name(&self) -> &'static str {
        "engine"
    }

    fn generation(&self) -> PartGeneration {
        u64::from(self.state.get().agent_id_allocator.peek().0)
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) fn checkpoint_session_runtime(
        &self,
        session_id: SessionId,
    ) -> Result<(), RuntimeCheckpointError> {
        if !self.sessions.contains(session_id) {
            return Ok(());
        }
        let engine_snapshot = DurablePartSnapshot::capture(self)?;
        let drive_snapshot = {
            let drives = self.drives.borrow();
            let Some(drive) = drives.get(&session_id) else {
                return Ok(());
            };
            DurablePartSnapshot::capture(drive)?
        };
        let instance_snapshot = {
            let instances = self.instances.borrow();
            if let Some(instance) = instances.get(&session_id) {
                Some(DurablePartSnapshot::capture(instance)?)
            } else {
                None
            }
        };
        let mut session_parts = vec![drive_snapshot];
        if let Some(instance_snapshot) = instance_snapshot {
            session_parts.push(instance_snapshot);
        }

        self.checkpoints.checkpoint_now(vec![
            DurableBatchSnapshot::new(ENGINE_BATCH, ENGINE_BATCH_ID, vec![engine_snapshot]),
            DurableBatchSnapshot::new(
                SESSION_RUNTIME_BATCH,
                BatchId::new(session_id.0),
                session_parts,
            ),
        ])?;
        Ok(())
    }
}
use std::borrow::Cow;
