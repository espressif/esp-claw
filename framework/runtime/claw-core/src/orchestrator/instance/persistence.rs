mod codec;
mod error;
mod schema;

use std::collections::BTreeMap;

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, PartGeneration, PartStateBlob, StorageHint,
    StorageSizeHint,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use super::state::OrchestratorInstanceState;
use super::OrchestratorInstance;

pub(crate) use error::OrchestratorInstanceRestoreError;
pub(in crate::orchestrator::instance) use schema::AgentPartState;

pub(crate) struct OrchestratorInstanceRestore {
    pub(super) state: OrchestratorInstanceState,
    pub(super) agent_parts: BTreeMap<crate::agent::AgentId, Vec<AgentPartState>>,
}

impl<Filesystem, Http, Timer> DurablePart for OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn name(&self) -> &'static str {
        "orchestrator-instance"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        codec::encode_checkpoint(self.state.get(), &self.registry)
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Large,
            change: ChangePatternHint::Arbitrary,
        }
    }
}
