use std::borrow::Cow;

use claw_checkpoint::{
    BatchId, BatchWrite, ChangePatternHint, CheckpointStorage, CheckpointWrite, DurablePart,
    DurablePartError, DurableStateCodec, FsCheckpointStorage, PartGeneration, PartStateBlob,
    PartStateSlice, PartWrite, StorageHint, StorageSizeHint,
};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use serde::{Deserialize, Serialize};

use crate::agent::AgentIdAllocator;
use crate::session::SessionId;

use super::super::checkpoint::RuntimeCheckpointError;
use super::super::{
    ENGINE_BATCH, ENGINE_BATCH_ID, ENGINE_PART, ORCHESTRATOR_INSTANCE_PART, SESSION_DRIVE_PART,
    SESSION_RUNTIME_BATCH,
};
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
    Http: ClawHttp + Default + 'static,
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
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) fn checkpoint_session_runtime(
        &self,
        session_id: SessionId,
    ) -> Result<(), RuntimeCheckpointError> {
        let engine_state = self.export_state()?;
        let engine_hint = self.storage_hint();
        let drive_write = {
            let drives = self.drives.borrow();
            let Some(drive) = drives.get(&session_id) else {
                return Ok(());
            };
            let state = drive.export_state()?;
            PartWrite {
                name: SESSION_DRIVE_PART,
                state: PartStateBlob {
                    schema_version: state.schema_version,
                    bytes: Cow::Owned(state.bytes.into_owned()),
                },
                hint: drive.storage_hint(),
            }
        };
        let instance_write = {
            let instances = self.instances.borrow();
            if let Some(instance) = instances.get(&session_id) {
                let state = instance.export_state()?;
                Some(PartWrite {
                    name: ORCHESTRATOR_INSTANCE_PART,
                    state: PartStateBlob {
                        schema_version: state.schema_version,
                        bytes: Cow::Owned(state.bytes.into_owned()),
                    },
                    hint: instance.storage_hint(),
                })
            } else {
                None
            }
        };
        let mut session_writes = vec![drive_write];
        if let Some(instance_write) = instance_write {
            session_writes.push(instance_write);
        }

        let mut storage = FsCheckpointStorage::<Filesystem>::new(self.checkpoint_dir.clone());
        let step = storage.next_step()?;
        storage.write_checkpoint(CheckpointWrite {
            step,
            batches: vec![
                BatchWrite {
                    batch: (ENGINE_BATCH, ENGINE_BATCH_ID),
                    writes: vec![PartWrite {
                        name: ENGINE_PART,
                        state: engine_state,
                        hint: engine_hint,
                    }],
                },
                BatchWrite {
                    batch: (SESSION_RUNTIME_BATCH, BatchId::new(session_id.0)),
                    writes: session_writes,
                },
            ],
        })?;
        Ok(())
    }
}
