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
use super::session_io::{apply_control, ControlOp};

const SESSION_DRIVE_SCHEMA_VERSION: u32 = 2;

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SessionDrivePhase {
    Closed,
    OpenIdle,
    OpenDriving,
    Closing,
}

pub(in crate::orchestrator) struct SessionDrive {
    phase: SessionDrivePhase,
    events: Option<EventSink>,
    foreground_active: bool,
    control: Option<DriveControl>,
    requested_control: Option<ControlOp>,
    close_cancels: bool,
    /// Event delivery is process-local. A restored turn is announced again on
    /// the newly opened stream.
    announced_turn: Option<TurnId>,
    control_acks: Vec<Sender<Result<(), SessionControlError>>>,
    close_acks: Vec<Sender<Result<(), SessionControlError>>>,
    state: DurableState<SessionDriveState>,
}

impl SessionDrive {
    pub(in crate::orchestrator) fn new(state: SessionDriveState) -> Self {
        Self {
            phase: SessionDrivePhase::Closed,
            events: None,
            foreground_active: false,
            control: None,
            requested_control: None,
            close_cancels: false,
            announced_turn: None,
            control_acks: Vec::new(),
            close_acks: Vec::new(),
            state: DurableState::new(state),
        }
    }

    #[cfg(test)]
    fn phase(&self) -> SessionDrivePhase {
        self.phase
    }

    /// Attach the process-local event stream to a restored or fresh drive.
    /// Returns whether durable input was waiting when the stream opened.
    pub(super) fn open(&mut self, events: EventSink) -> Option<bool> {
        if self.phase != SessionDrivePhase::Closed {
            return None;
        }
        self.events = Some(events);
        self.announced_turn = None;
        self.close_cancels = false;
        self.phase = SessionDrivePhase::OpenIdle;
        Some(self.has_pending_input())
    }

    pub(super) fn is_open(&self) -> bool {
        matches!(
            self.phase,
            SessionDrivePhase::OpenIdle | SessionDrivePhase::OpenDriving
        )
    }

    pub(super) fn is_closing(&self) -> bool {
        self.phase == SessionDrivePhase::Closing
    }

    pub(super) fn event_sink(&self) -> Option<EventSink> {
        self.events.clone()
    }

    /// Move an idle open drive into its one in-flight drive slot.
    pub(super) fn start(&mut self) -> bool {
        if self.phase != SessionDrivePhase::OpenIdle {
            return false;
        }
        self.phase = SessionDrivePhase::OpenDriving;
        true
    }

    pub(super) fn finish_drive(&mut self) {
        debug_assert_eq!(self.phase, SessionDrivePhase::OpenDriving);
        if self.phase == SessionDrivePhase::OpenDriving {
            self.phase = SessionDrivePhase::OpenIdle;
        }
        self.foreground_active = false;
        self.control = None;
        self.requested_control = None;
    }

    pub(super) fn request_close(&mut self, has_root_work: bool) -> bool {
        if self.phase == SessionDrivePhase::Closing {
            return false;
        }
        let was_driving = self.phase == SessionDrivePhase::OpenDriving;
        let had_pending_input = self.take_pending_input().is_some();
        let had_active_turn = self.has_active_turn();
        self.close_cancels = self.close_cancels
            || was_driving
            || self.foreground_active
            || had_pending_input
            || had_active_turn
            || has_root_work;
        self.phase = SessionDrivePhase::Closing;
        self.requested_control = Some(ControlOp::Cancel);
        if let Some(control) = &self.control {
            apply_control(control, self.requested_control);
        }
        !was_driving
    }

    pub(super) fn close_cancels(&self) -> bool {
        self.close_cancels
    }

    pub(super) fn finish_close(&mut self) {
        debug_assert_eq!(self.phase, SessionDrivePhase::Closing);
        self.events = None;
        let _ = self.take_pending_input();
        self.foreground_active = false;
        self.control = None;
        self.requested_control = None;
        self.close_cancels = false;
        self.announced_turn = None;
        self.phase = SessionDrivePhase::Closed;
    }

    pub(super) fn set_foreground_active(&mut self, active: bool) {
        debug_assert!(!active || self.phase == SessionDrivePhase::OpenDriving);
        self.foreground_active = active;
    }

    pub(super) fn is_foreground_active(&self) -> bool {
        self.foreground_active
    }

    pub(super) fn request_wake(&self) {
        if let Some(control) = &self.control {
            control.request_wake();
        }
    }

    pub(super) fn request_control(&mut self, op: ControlOp) {
        let requested = ControlOp::merge(self.requested_control, op);
        self.requested_control = Some(requested);
        if let Some(control) = &self.control {
            apply_control(control, self.requested_control);
        }
    }

    pub(super) fn take_requested_control(&mut self) -> Option<ControlOp> {
        self.requested_control.take()
    }

    pub(super) fn set_active_control(&mut self, control: Option<DriveControl>) {
        if let Some(control) = &control {
            apply_control(control, self.requested_control);
        } else {
            self.requested_control = None;
        }
        self.control = control;
    }

    pub(super) fn take_unannounced_turn(&mut self) -> Option<TurnId> {
        let turn = self.active_turn_id()?;
        if self.announced_turn == Some(turn) {
            return None;
        }
        self.announced_turn = Some(turn);
        Some(turn)
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

#[cfg(test)]
mod tests {
    use super::{SessionDrive, SessionDrivePhase, SessionDriveState};
    use crate::event::EventSink;
    use claw_checkpoint::DurablePart;

    #[test]
    fn lifecycle_has_one_explicit_phase() {
        let mut drive = SessionDrive::new(SessionDriveState::default());

        assert_eq!(drive.phase(), SessionDrivePhase::Closed);
        assert!(!drive.open(EventSink::disabled()).unwrap());
        assert_eq!(drive.phase(), SessionDrivePhase::OpenIdle);

        assert!(drive.start());
        assert!(!drive.start());
        assert_eq!(drive.phase(), SessionDrivePhase::OpenDriving);

        assert!(!drive.request_close(false));
        assert_eq!(drive.phase(), SessionDrivePhase::Closing);
        assert!(!drive.start());

        drive.finish_close();
        assert_eq!(drive.phase(), SessionDrivePhase::Closed);
        assert!(drive.event_sink().is_none());
    }

    #[test]
    fn closing_an_idle_drive_schedules_exactly_once() {
        let mut drive = SessionDrive::new(SessionDriveState::default());
        drive.open(EventSink::disabled()).unwrap();

        assert!(drive.request_close(false));
        assert!(!drive.request_close(false));
        assert_eq!(drive.phase(), SessionDrivePhase::Closing);
    }

    #[test]
    fn a_completed_drive_releases_the_only_drive_slot() {
        let mut drive = SessionDrive::new(SessionDriveState::default());
        drive.open(EventSink::disabled()).unwrap();

        assert!(drive.start());
        drive.finish_drive();

        assert_eq!(drive.phase(), SessionDrivePhase::OpenIdle);
        assert!(drive.start());
    }

    #[test]
    fn checkpoint_round_trips_the_current_schema() {
        let drive = SessionDrive::new(SessionDriveState::default());

        assert_eq!(drive.name(), "session-drive");
        let state = drive.export_state().unwrap().into_owned();
        assert_eq!(state.schema_version, 2);

        let restored = SessionDrive::restore_from_state(state.as_slice()).unwrap();
        assert_eq!(restored.name(), "session-drive");
        assert_eq!(restored.phase(), SessionDrivePhase::Closed);
    }
}
