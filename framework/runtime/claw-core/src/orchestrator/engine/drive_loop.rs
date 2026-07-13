use std::rc::Rc;

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use tracing::Instrument as _;

use crate::event::{EventSink, SessionEvent};
use crate::orchestrator::control::DriveStop;
use crate::session::{Message, SessionId};

use super::super::control::DriveControl;
use super::super::instance::TurnStopMode;
use super::session_io::{apply_control, ControlOp};
use super::{DriveFuture, Engine, InstanceWork};

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) fn session_shutdown(self: &Rc<Self>, session_id: SessionId) -> Option<DriveFuture> {
        let has_root_work = self.instance_has_root_work(session_id);
        let mut should_start = false;
        {
            let mut drives = self.drives.borrow_mut();
            let drive = drives.get_mut(&session_id)?;
            let had_pending_input = drive.take_pending_input().is_some();
            let had_active_turn = drive.has_active_turn();
            drive.close_cancels = drive.close_cancels
                || drive.running
                || drive.foreground_active
                || had_pending_input
                || had_active_turn
                || has_root_work;
            drive.closing = true;
            drive.requested_control = Some(ControlOp::Cancel);
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
            if !drive.running {
                drive.running = true;
                should_start = true;
            }
        }
        should_start.then(|| {
            let engine = Rc::clone(self);
            Box::pin(
                async move {
                    engine.drive_session(session_id).await;
                }
                .instrument(tracing::info_span!("session", run.session = %session_id)),
            ) as DriveFuture
        })
    }

    pub(super) fn ensure_session_drive(
        self: &Rc<Self>,
        session_id: SessionId,
    ) -> Option<DriveFuture> {
        {
            let mut drives = self.drives.borrow_mut();
            let drive = drives.get_mut(&session_id)?;
            if drive.running {
                if let Some(control) = &drive.control {
                    control.request_wake();
                }
                return None;
            }
            drive.running = true;
        }
        let engine = Rc::clone(self);
        Some(Box::pin(
            async move {
                engine.drive_session(session_id).await;
            }
            .instrument(tracing::info_span!("session", run.session = %session_id)),
        ))
    }

    async fn drive_session(&self, session_id: SessionId) {
        loop {
            if self.session_is_closing(session_id) {
                self.finish_closing_session(session_id).await;
                return;
            }

            self.announce_active_turn(session_id);

            let requested_control = self
                .drives
                .borrow_mut()
                .get_mut(&session_id)
                .and_then(|drive| drive.requested_control.take());
            if let Some(op) = requested_control {
                let mode = match op {
                    ControlOp::Interrupt => TurnStopMode::PreserveAgents,
                    ControlOp::Cancel => TurnStopMode::DeleteSpawnedAgents,
                };
                if let Some(mut slot) = self.checkout_existing_instance(session_id) {
                    slot.get_mut().stop_turn_tasks(mode).await;
                }
                self.finish_active_turn(session_id);
                break;
            }

            if let Some(input) = self.take_input(session_id) {
                let stop = self.drive_user_turn(session_id, input).await;
                if stop != DriveStop::Quiescent {
                    self.finish_active_turn(session_id);
                    if self.session_is_closing(session_id) {
                        continue;
                    }
                    break;
                }
                continue;
            }

            match self.instance_work(session_id) {
                InstanceWork::Root => {
                    let stop = self.drive_root_ready_for_active_turn(session_id).await;
                    if stop != DriveStop::Quiescent {
                        self.finish_active_turn(session_id);
                        if self.session_is_closing(session_id) {
                            continue;
                        }
                        break;
                    }
                    continue;
                }
                InstanceWork::Background => {
                    let stop = self.drive_background(session_id).await;
                    if stop != DriveStop::Quiescent {
                        self.finish_active_turn(session_id);
                        if self.session_is_closing(session_id) {
                            continue;
                        }
                        break;
                    }
                    continue;
                }
                InstanceWork::None => {
                    if self.instance_has_active_approval(session_id) {
                        break;
                    }
                    self.finish_active_turn(session_id);
                }
            }

            break;
        }
        self.finish_session_drive(session_id);
    }

    fn session_is_closing(&self, session_id: SessionId) -> bool {
        match self.drives.borrow().get(&session_id) {
            Some(drive) => drive.closing,
            None => true,
        }
    }

    fn take_input(&self, session_id: SessionId) -> Option<Message> {
        let mut drives = self.drives.borrow_mut();
        let drive = drives.get_mut(&session_id)?;
        if drive.closing {
            return None;
        }
        drive.take_pending_input()
    }

    pub(super) fn session_events(&self, session_id: SessionId) -> Option<EventSink> {
        self.drives
            .borrow()
            .get(&session_id)
            .and_then(|drive| drive.events.clone())
    }

    fn finish_session_drive(&self, session_id: SessionId) {
        let mut drives = self.drives.borrow_mut();
        if let Some(drive) = drives.get_mut(&session_id) {
            drive.running = false;
            drive.foreground_active = false;
            drive.control = None;
            drive.requested_control = None;
        }
    }

    pub(super) fn set_foreground_active(&self, session_id: SessionId, active: bool) {
        if let Some(drive) = self.drives.borrow_mut().get_mut(&session_id) {
            drive.foreground_active = active;
        }
    }

    pub(super) fn set_active_control(&self, session_id: SessionId, control: Option<DriveControl>) {
        let mut drives = self.drives.borrow_mut();
        if let Some(drive) = drives.get_mut(&session_id) {
            if let Some(control) = &control {
                apply_control(control, drive.requested_control);
            } else {
                drive.requested_control = None;
            }
            drive.control = control;
        }
    }

    async fn finish_closing_session(&self, session_id: SessionId) {
        let (events, should_cancel, deleted) = {
            let drives = self.drives.borrow();
            let Some(drive) = drives.get(&session_id) else {
                return;
            };
            (
                drive.events.clone(),
                drive.close_cancels,
                !self.sessions.contains(session_id),
            )
        };

        if should_cancel {
            if let Some(mut slot) = self.checkout_existing_instance(session_id) {
                slot.get_mut()
                    .stop_turn_tasks(TurnStopMode::DeleteSpawnedAgents)
                    .await;
            }
            self.finish_active_turn(session_id);
        }

        if let Some(events) = events {
            tracing::info!(name: "closed", "");
            events.emit(SessionEvent::Closed);
        }

        if deleted {
            let close_acks = {
                let mut drives = self.drives.borrow_mut();
                let Some(drive) = drives.get_mut(&session_id) else {
                    return;
                };
                drive.take_close_acks()
            };
            tracing::info_span!("session.delete", run.session = %session_id).in_scope(|| {
                tracing::info!(name: "runtime_state_removed", "");
            });
            self.drives.borrow_mut().remove(&session_id);
            self.instances.borrow_mut().remove(&session_id);
            for ack in close_acks {
                let _ = ack.try_send(Ok(()));
            }
            return;
        }

        if let Some(drive) = self.drives.borrow_mut().get_mut(&session_id) {
            drive.events = None;
            let _ = drive.take_pending_input();
            drive.running = false;
            drive.foreground_active = false;
            drive.control = None;
            drive.requested_control = None;
            drive.closing = false;
            drive.close_cancels = false;
        }
        let close_result = match self.checkpoint_session_runtime(session_id) {
            Ok(()) => Ok(()),
            Err(error) => {
                tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
                Err(crate::orchestrator::SessionControlError::ClosePersistence)
            }
        };
        let close_acks = {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                return;
            };
            drive.take_close_acks()
        };
        for ack in close_acks {
            let _ = ack.try_send(close_result.clone());
        }
    }
}
