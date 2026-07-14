use std::borrow::Cow;
use std::sync::{Arc, Mutex};

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, PartGeneration, PartStateBlob,
    PartStateSlice, StorageHint, StorageSizeHint,
};
use serde::{Deserialize, Serialize};

use crate::protocol::AgentId;

crate::define_id_allocator!(AgentIdCounter(AgentId), AgentId(1));

/// Process-wide allocator shared by all multiagent runtimes.
#[derive(Clone, Debug)]
pub(crate) struct AgentIdAllocator(Arc<Mutex<AgentIdCounter>>);

impl AgentIdAllocator {
    pub(crate) fn new() -> Self {
        Self(Arc::new(Mutex::new(AgentIdCounter::new())))
    }

    fn starting_at(first: AgentId) -> Self {
        Self(Arc::new(Mutex::new(AgentIdCounter::starting_at(first))))
    }

    pub(crate) fn next(&self) -> AgentId {
        self.0
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .next()
    }

    pub(crate) fn peek(&self) -> AgentId {
        self.0.lock().unwrap_or_else(|poison| poison.into_inner()).0
    }
}

impl Serialize for AgentIdAllocator {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.peek().serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for AgentIdAllocator {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let next = AgentId::deserialize(deserializer)?;
        Ok(Self::starting_at(next))
    }
}

impl DurablePart for AgentIdAllocator {
    fn name(&self) -> &'static str {
        "agent-id-allocator"
    }

    fn generation(&self) -> PartGeneration {
        u64::from(self.peek().0)
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        if state.schema_version != 1 {
            return Err(DurablePartError::InvalidState(
                "unsupported agent-id-allocator checkpoint schema",
            ));
        }
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}
