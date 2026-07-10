use std::borrow::Cow;

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use serde::{Deserialize, Serialize};

use crate::event::EventSink;
use crate::session::{TurnId, TurnIdAllocator};

use super::super::control::DriveControl;
use super::session_io::ControlOp;

/// One accepted user input waiting to be handed to the session pump.
#[derive(Deserialize, Serialize)]
pub(super) struct SubmittedInput {
    pub(super) text: String,
}

#[derive(Deserialize, Serialize)]
pub(in crate::orchestrator) struct SessionDriveState {
    pending_input: Option<SubmittedInput>,
    next_turn_id: TurnId,
}

impl Default for SessionDriveState {
    fn default() -> Self {
        Self {
            pending_input: None,
            next_turn_id: TurnIdAllocator::new().peek(),
        }
    }
}

impl DurableStateCodec for SessionDriveState {
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

pub(in crate::orchestrator) struct SessionDrive {
    pub(super) events: Option<EventSink>,
    pub(super) running: bool,
    pub(super) foreground_active: bool,
    pub(super) control: Option<DriveControl>,
    pub(super) requested_control: Option<ControlOp>,
    pub(super) closing: bool,
    pub(super) close_cancels: bool,
    state: DurableState<SessionDriveState>,
}

impl SessionDrive {
    pub(in crate::orchestrator) fn new(state: SessionDriveState) -> Self {
        Self {
            events: None,
            running: false,
            foreground_active: false,
            control: None,
            requested_control: None,
            closing: false,
            close_cancels: false,
            state: DurableState::new(state),
        }
    }

    pub(super) fn has_pending_input(&self) -> bool {
        self.state.get().pending_input.is_some()
    }

    pub(super) fn set_pending_input(&mut self, input: SubmittedInput) {
        self.state.get_mut().pending_input = Some(input);
    }

    pub(super) fn take_pending_input(&mut self) -> Option<SubmittedInput> {
        self.state.get().pending_input.as_ref()?;
        self.state.get_mut().pending_input.take()
    }

    pub(super) fn next_turn(&mut self) -> TurnId {
        let state = self.state.get_mut();
        let turn = state.next_turn_id;
        state.next_turn_id = TurnId::new(turn.0.saturating_add(1));
        turn
    }
}

impl DurablePart for SessionDrive {
    fn name(&self) -> &'static str {
        "session-drive"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(Self::new(SessionDriveState::decode_state(state)?))
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}
