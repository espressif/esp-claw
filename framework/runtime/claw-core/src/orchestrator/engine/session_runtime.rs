use async_channel::Sender;
use claw_checkpoint::{DurablePartError, DurablePartSnapshot};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::event::EventSink;
use crate::orchestrator::{ReasoningEffort, SessionControlError};
use crate::session::{Message, TurnId};

use super::super::control::DriveControl;
use super::super::instance::OrchestratorInstance;
use super::session_drive::{SessionDrive, SessionDriveState};
use super::session_io::ControlOp;
use super::InstanceWork;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum InputRejection {
    Closed,
    Busy,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum ControlRequest {
    Closed,
    Idle,
    Queued,
}

/// The one engine-level owner of a session's drive and agent-instance state.
///
/// A session always has exactly one drive. Its agent instance is created lazily
/// and temporarily checked out while async work is polled, but it is never kept
/// in a second engine-level map.
pub(in crate::orchestrator) struct SessionRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    drive: SessionDrive,
    instance: Option<OrchestratorInstance<Filesystem, Http, Timer>>,
}

impl<Filesystem, Http, Timer> SessionRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn new(
        drive_state: SessionDriveState,
        instance: Option<OrchestratorInstance<Filesystem, Http, Timer>>,
    ) -> Self {
        Self {
            drive: SessionDrive::new(drive_state),
            instance,
        }
    }

    pub(super) fn fresh() -> Self {
        Self::new(SessionDriveState::default(), None)
    }

    pub(in crate::orchestrator) fn from_restored_parts(
        drive_state: SessionDriveState,
        instance: Option<OrchestratorInstance<Filesystem, Http, Timer>>,
    ) -> Self {
        Self::new(drive_state, instance)
    }

    pub(super) fn take_instance(
        &mut self,
    ) -> Option<OrchestratorInstance<Filesystem, Http, Timer>> {
        self.instance.take()
    }

    pub(super) fn put_instance(&mut self, instance: OrchestratorInstance<Filesystem, Http, Timer>) {
        debug_assert!(self.instance.is_none());
        self.instance = Some(instance);
    }

    pub(super) fn work(&self) -> InstanceWork {
        self.instance
            .as_ref()
            .map_or(InstanceWork::None, OrchestratorInstance::work)
    }

    pub(super) fn has_active_approval(&self) -> bool {
        self.instance
            .as_ref()
            .is_some_and(|instance| instance.active_approval().is_some())
    }

    pub(super) fn open(&mut self, events: EventSink) -> Option<bool> {
        self.drive.open(events)
    }

    pub(super) fn is_open(&self) -> bool {
        self.drive.is_open()
    }

    pub(super) fn is_closing(&self) -> bool {
        self.drive.is_closing()
    }

    pub(super) fn set_reasoning_effort(&mut self, effort: ReasoningEffort) {
        self.drive.set_reasoning_effort(effort);
    }

    pub(super) fn accept_input(&mut self, input: Message) -> Result<(), InputRejection> {
        if !self.drive.is_open() {
            return Err(InputRejection::Closed);
        }
        let continues_approval = self.drive.has_active_turn() && self.has_active_approval();
        if self.drive.has_pending_input()
            || (self.drive.has_active_turn() && !continues_approval)
            || self.drive.is_foreground_active()
            || self.work() == InstanceWork::Root
        {
            return Err(InputRejection::Busy);
        }
        if continues_approval {
            self.drive.continue_turn(input);
        } else {
            self.drive.begin_turn(input);
        }
        self.drive.request_wake();
        Ok(())
    }

    pub(super) fn request_control(&mut self, op: ControlOp) -> ControlRequest {
        if !self.drive.is_open() {
            return ControlRequest::Closed;
        }
        if !self.drive.has_active_turn() {
            return ControlRequest::Idle;
        }
        let _ = self.drive.take_pending_input();
        self.drive.request_control(op);
        ControlRequest::Queued
    }

    pub(super) fn queue_control_ack(&mut self, ack: Sender<Result<(), SessionControlError>>) {
        self.drive.queue_control_ack(ack);
    }

    pub(super) fn queue_close_ack(&mut self, ack: Sender<Result<(), SessionControlError>>) {
        self.drive.queue_close_ack(ack);
    }

    pub(super) fn request_close(&mut self) -> bool {
        let has_root_work = self.work() == InstanceWork::Root;
        self.drive.request_close(has_root_work)
    }

    pub(super) fn ensure_drive_started(&mut self) -> bool {
        if self.drive.start() {
            return true;
        }
        self.drive.request_wake();
        false
    }

    pub(super) fn take_requested_control(&mut self) -> Option<ControlOp> {
        self.drive.take_requested_control()
    }

    pub(super) fn take_input(&mut self) -> Option<Message> {
        if self.drive.is_closing() {
            return None;
        }
        self.drive.take_pending_input()
    }

    pub(super) fn event_sink(&self) -> Option<EventSink> {
        self.drive.event_sink()
    }

    pub(super) fn finish_drive(&mut self) {
        self.drive.finish_drive();
    }

    pub(super) fn set_foreground_active(&mut self, active: bool) {
        self.drive.set_foreground_active(active);
    }

    pub(super) fn set_active_control(&mut self, control: Option<DriveControl>) {
        self.drive.set_active_control(control);
    }

    pub(super) fn close_snapshot(&self) -> (Option<EventSink>, bool) {
        (self.drive.event_sink(), self.drive.close_cancels())
    }

    pub(super) fn take_close_acks(&mut self) -> Vec<Sender<Result<(), SessionControlError>>> {
        self.drive.take_close_acks()
    }

    pub(super) fn finish_close(&mut self) {
        self.drive.finish_close();
    }

    pub(super) fn take_unannounced_turn(&mut self) -> Option<TurnId> {
        self.drive.take_unannounced_turn()
    }

    pub(super) fn active_turn_context(&self) -> Option<(TurnId, ReasoningEffort)> {
        self.drive
            .active_turn_id()
            .map(|turn| (turn, self.drive.reasoning_effort()))
    }

    pub(super) fn finish_turn(
        &mut self,
    ) -> (Option<TurnId>, Vec<Sender<Result<(), SessionControlError>>>) {
        (self.drive.finish_turn(), self.drive.take_control_acks())
    }

    pub(super) fn capture_checkpoint_parts(
        &self,
    ) -> Result<(DurablePartSnapshot, Option<DurablePartSnapshot>), DurablePartError> {
        let drive = DurablePartSnapshot::capture(&self.drive)?;
        let instance = self
            .instance
            .as_ref()
            .map(|instance| DurablePartSnapshot::capture(instance))
            .transpose()?;
        Ok((drive, instance))
    }
}

#[cfg(test)]
mod tests {
    use super::{InputRejection, SessionRuntime};
    use crate::event::EventSink;
    use crate::session::Message;
    use claw_interface::{ImmediateTimer, MemFs, RealHttp};

    type TestRuntime = SessionRuntime<MemFs, RealHttp, ImmediateTimer>;

    #[test]
    fn input_acceptance_is_owned_by_the_session_runtime() {
        let mut runtime = TestRuntime::fresh();

        assert_eq!(
            runtime.accept_input(Message::text("before open")),
            Err(InputRejection::Closed)
        );

        runtime.open(EventSink::disabled()).unwrap();
        assert_eq!(runtime.accept_input(Message::text("first")), Ok(()));
        assert_eq!(
            runtime.accept_input(Message::text("second")),
            Err(InputRejection::Busy)
        );
    }
}
