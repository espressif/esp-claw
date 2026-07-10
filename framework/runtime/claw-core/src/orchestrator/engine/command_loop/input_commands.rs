use std::rc::Rc;

use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::orchestrator::SessionControlError;
use crate::session::SessionId;

use super::super::session_drive::SubmittedInput;
use super::super::session_io::{apply_control, ControlOp};
use super::super::{DriveFuture, Engine};

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::engine::command_loop) fn submit_input(
        self: &Rc<Self>,
        session_id: SessionId,
        input: SubmittedInput,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let has_text = !input.text.is_empty();
        let text_bytes = input.text.len() as u64;
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "submit_rejected",
                reason = "session_closed",
                has_text,
                text_bytes,
                attachment_count = 0_u64,
                attachment_kinds = "none",
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
                    attachment_count = 0_u64,
                    attachment_kinds = "none",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "session_closed",
                    has_text,
                    text_bytes,
                    attachment_count = 0_u64,
                    attachment_kinds = "none",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if drive.has_pending_input()
                || drive.foreground_active
                || self.instance_has_root_work(session_id)
            {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "busy",
                    has_text,
                    text_bytes,
                    attachment_count = 0_u64,
                    attachment_kinds = "none",
                );
                return (Err(SessionControlError::Busy(session_id)), None);
            }
            drive.set_pending_input(input);
            if let Some(control) = &drive.control {
                control.request_wake();
            }
        }
        tracing::info!(
            name: "submit_accepted",
            has_text,
            text_bytes,
            attachment_count = 0_u64,
            attachment_kinds = "none",
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
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let op_name: &'static str = (&op).into();
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "control_rejected",
                op = op_name,
                reason = "session_closed",
            );
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut drives = self.drives.borrow_mut();
            let Some(drive) = drives.get_mut(&session_id) else {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if drive.events.is_none() || drive.closing {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            }
            if op == ControlOp::Cancel {
                let _ = drive.take_pending_input();
            }
            drive.requested_control = Some(ControlOp::merge(drive.requested_control, op));
            if let Some(control) = &drive.control {
                apply_control(control, drive.requested_control);
            }
        }
        tracing::info!(name: "control_requested", op = op_name);
        (Ok(()), self.ensure_session_drive(session_id))
    }
}
