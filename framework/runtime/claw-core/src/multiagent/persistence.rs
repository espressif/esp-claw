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

use crate::protocol::{AgentId, Message};

use super::{MultiagentRuntime, MultiagentState};

pub(crate) use error::MultiagentRestoreError;
pub(in crate::multiagent) use schema::AgentPartState;

pub(super) struct RestoredAgentSlot {
    pub(super) inbox: Vec<Message>,
    pub(super) parts: Vec<AgentPartState>,
}

pub(crate) struct MultiagentRestore {
    pub(super) state: MultiagentState,
    pub(super) agent_slots: BTreeMap<AgentId, RestoredAgentSlot>,
}

impl<Filesystem, Http, Timer> DurablePart for MultiagentRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn name(&self) -> &'static str {
        "multiagent-runtime"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        codec::encode_checkpoint(self.state.get(), &self.slots)
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Large,
            change: ChangePatternHint::Arbitrary,
        }
    }
}
