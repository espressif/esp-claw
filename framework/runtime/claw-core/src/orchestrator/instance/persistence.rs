mod codec;
mod error;
mod schema;

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, PartGeneration, PartStateBlob, StorageHint,
    StorageSizeHint,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use super::OrchestratorInstance;

pub use error::OrchestratorInstanceRestoreError;
pub(in crate::orchestrator::instance) use schema::AgentPartState;

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
        self.state.export_state()
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Large,
            change: ChangePatternHint::Arbitrary,
        }
    }
}
