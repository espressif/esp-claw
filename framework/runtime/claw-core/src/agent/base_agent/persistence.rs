use std::borrow::Cow;

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use claw_interface::{ClawHttp, ClawTimer};

use super::state::BaseAgentState;
use super::BaseAgent;

impl DurableStateCodec for BaseAgentState {
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

impl<H: ClawHttp, Timer: ClawTimer> DurablePart for BaseAgent<H, Timer> {
    fn name(&self) -> &'static str {
        "base-agent"
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

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    pub(crate) fn restore_state(
        &mut self,
        state: PartStateSlice<'_>,
    ) -> Result<(), DurablePartError> {
        self.state = DurableState::restore_state(state)?;
        self.outcome = None;
        self.interruption.clear();
        Ok(())
    }

    pub(crate) fn durable_parts(&self) -> Vec<&dyn DurablePart> {
        vec![self, &self.tools]
    }

    pub(crate) fn restore_durable_part(
        &mut self,
        name: &str,
        state: PartStateSlice<'_>,
    ) -> Result<bool, DurablePartError> {
        match name {
            "base-agent" => {
                self.restore_state(state)?;
                Ok(true)
            }
            "tool-set" => {
                self.tools.restore_state(state)?;
                Ok(true)
            }
            _ => Ok(false),
        }
    }
}
