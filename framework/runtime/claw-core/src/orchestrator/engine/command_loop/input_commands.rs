use std::rc::Rc;

use async_channel::Sender;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::orchestrator::SessionControlError;
use crate::session::{Message, SessionId};

use super::super::session_io::{apply_control, ControlOp};
use super::super::{DriveFuture, Engine};

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::engine::command_loop) fn submit_input(
        self: &Rc<Self>,
        session_id: SessionId,
        input: Message,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let has_text = input.text.as_ref().is_some_and(|text| !text.is_empty());
        let text_bytes = input.text.as_ref().map_or(0, |text| text.len()) as u64;
        let attachment_count = input.attachments.len() as u64;
        let attachment_kinds = if input.attachments.is_empty() {
            "none"
        } else {
            "references"
        };
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "submit_rejected",
                reason = "session_closed",
                has_text,
                text_bytes,
                attachment_count,
                attachment_kinds,
            );
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "session_closed",
                    has_text,
                    text_bytes,
                    attachment_count,
                    attachment_kinds,
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "session_closed",
                    has_text,
                    text_bytes,
                    attachment_count,
                    attachment_kinds,
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            let continues_approval =
                drive.has_active_turn() && self.instance_has_active_approval(session_id);
            if drive.has_pending_input()
                || (drive.has_active_turn() && !continues_approval)
                || drive.foreground_active
                || self.instance_has_root_work(session_id)
            {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "busy",
                    has_text,
                    text_bytes,
                    attachment_count,
                    attachment_kinds,
                );
                return (Err(SessionControlError::Busy(session_id)), None);
            }
            if continues_approval {
                drive.continue_turn(input);
            } else {
                drive.begin_turn(input);
            }
            if let Some(control) = &drive.control {
                control.request_wake();
            }
        }
        tracing::info!(
            name: "submit_accepted",
            has_text,
            text_bytes,
            attachment_count,
            attachment_kinds,
        );
        if let Err(error) = self.checkpoint_session_runtime(session_id) {
            tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
        }
        (Ok(()), self.ensure_session_drive(session_id))
    }

    pub(in crate::orchestrator::engine::command_loop) fn control_session(
        self: &Rc<Self>,
        session_id: SessionId,
        op: ControlOp,
        ack: Sender<Result<(), SessionControlError>>,
    ) -> Option<DriveFuture> {
        let op_name: &'static str = (&op).into();
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "control_rejected",
                op = op_name,
                reason = "session_closed",
            );
            let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
            return None;
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
                return None;
            };
            if drive.events.is_none() || drive.closing {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
                return None;
            }
            if !drive.has_active_turn() {
                let _ = ack.try_send(Ok(()));
                return None;
            }
            let _ = drive.take_pending_input();
            drive.requested_control = Some(ControlOp::merge(drive.requested_control, op));
            drive.queue_control_ack(ack);
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
        }
        tracing::info!(name: "control_requested", op = op_name);
        self.ensure_session_drive(session_id)
    }
}
