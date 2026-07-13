use std::borrow::Cow;

use async_channel::Sender;
use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use serde::{Deserialize, Serialize};

use crate::event::EventSink;
use crate::orchestrator::ReasoningEffort;
use crate::orchestrator::SessionControlError;
use crate::session::{Message, TurnId, TurnIdAllocator};

use super::super::control::DriveControl;
use super::session_io::ControlOp;

const SESSION_DRIVE_SCHEMA_VERSION: u32 = 1;

/// Durable work owned by the session's one active turn.
#[derive(Deserialize, Serialize)]
pub(super) struct TurnState {
    id: TurnId,
    pending_input: Option<Message>,
}

#[derive(Deserialize, Serialize)]
pub(in crate::orchestrator) struct SessionDriveState {
    active_turn: Option<TurnState>,
    next_turn_id: TurnId,
    /// Reasoning effort in force for the current turn.
    reasoning_effort: ReasoningEffort,
    /// A requested effort not yet in force; promoted to `reasoning_effort` at the
    /// next turn boundary so a running turn is never disrupted mid-flight.
    pending_reasoning_effort: Option<ReasoningEffort>,
}

impl Default for SessionDriveState {
    fn default() -> Self {
        Self {
            active_turn: None,
            next_turn_id: TurnIdAllocator::new().peek(),
            reasoning_effort: ReasoningEffort::default(),
            pending_reasoning_effort: None,
        }
    }
}

impl DurableStateCodec for SessionDriveState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: SESSION_DRIVE_SCHEMA_VERSION,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        if state.schema_version != SESSION_DRIVE_SCHEMA_VERSION {
            return Err(DurablePartError::InvalidState(
                "unsupported session-drive checkpoint schema",
            ));
        }
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
    /// Event delivery is process-local. A restored turn is announced again on
    /// the newly opened stream.
    pub(super) announced_turn: Option<TurnId>,
    pub(super) control_acks: Vec<Sender<Result<(), SessionControlError>>>,
    pub(super) close_acks: Vec<Sender<Result<(), SessionControlError>>>,
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
            announced_turn: None,
            control_acks: Vec::new(),
            close_acks: Vec::new(),
            state: DurableState::new(state),
        }
    }

    pub(super) fn has_active_turn(&self) -> bool {
        self.state.get().active_turn.is_some()
    }

    pub(super) fn active_turn_id(&self) -> Option<TurnId> {
        self.state.get().active_turn.as_ref().map(|turn| turn.id)
    }

    pub(super) fn has_pending_input(&self) -> bool {
        self.state
            .get()
            .active_turn
            .as_ref()
            .is_some_and(|turn| turn.pending_input.is_some())
    }

    pub(super) fn begin_turn(&mut self, input: Message) -> TurnId {
        let state = self.state.get_mut();
        if let Some(effort) = state.pending_reasoning_effort.take() {
            state.reasoning_effort = effort;
        }
        let id = state.next_turn_id;
        state.next_turn_id = TurnId::new(id.0.saturating_add(1));
        state.active_turn = Some(TurnState {
            id,
            pending_input: Some(input),
        });
        id
    }

    pub(super) fn continue_turn(&mut self, input: Message) {
        if let Some(turn) = self.state.get_mut().active_turn.as_mut() {
            turn.pending_input = Some(input);
        }
    }

    pub(super) fn take_pending_input(&mut self) -> Option<Message> {
        self.state
            .get_mut()
            .active_turn
            .as_mut()?
            .pending_input
            .take()
    }

    pub(super) fn finish_turn(&mut self) -> Option<TurnId> {
        let turn = self.state.get_mut().active_turn.take()?;
        self.announced_turn = None;
        Some(turn.id)
    }

    pub(super) fn queue_control_ack(&mut self, ack: Sender<Result<(), SessionControlError>>) {
        self.control_acks.push(ack);
    }

    pub(super) fn take_control_acks(&mut self) -> Vec<Sender<Result<(), SessionControlError>>> {
        std::mem::take(&mut self.control_acks)
    }

    pub(super) fn queue_close_ack(&mut self, ack: Sender<Result<(), SessionControlError>>) {
        self.close_acks.push(ack);
    }

    pub(super) fn take_close_acks(&mut self) -> Vec<Sender<Result<(), SessionControlError>>> {
        std::mem::take(&mut self.close_acks)
    }

    /// Queue a reasoning-effort change for this session. Held pending and applied
    /// at the next turn boundary (see [`begin_turn`](Self::begin_turn)), so the
    /// running turn is undisturbed.
    pub(super) fn set_reasoning_effort(&mut self, effort: ReasoningEffort) {
        self.state.get_mut().pending_reasoning_effort = Some(effort);
    }

    /// The reasoning effort in force for the current turn. A change requested
    /// mid-turn is not reflected until the next turn begins.
    ///
    pub(super) fn reasoning_effort(&self) -> ReasoningEffort {
        self.state.get().reasoning_effort
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
