use std::rc::Rc;

use async_channel::Sender;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::orchestrator::SessionControlError;
use crate::session::{Message, SessionId};

use super::super::session_io::ControlOp;
use super::super::session_runtime::{ControlRequest, InputRejection};
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
        let has_text = !input.as_str().is_empty();
        let text_bytes = input.as_str().len() as u64;
        let span = tracing::info_span!("session", run.session = %session_id);
        let _enter = span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(
                name: "submit_rejected",
                reason = "session_closed",
                has_text,
                text_bytes,
            );
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let mut runtimes = self.runtimes.borrow_mut();
            let Some(runtime) = runtimes.get_mut(&session_id) else {
                tracing::warn!(
                    name: "submit_rejected",
                    reason = "session_closed",
                    has_text,
                    text_bytes,
                );
                return (Err(SessionControlError::SessionClosed(session_id)), None);
            };
            if let Err(rejection) = runtime.accept_input(input) {
                let (reason, error) = match rejection {
                    InputRejection::Closed => (
                        "session_closed",
                        SessionControlError::SessionClosed(session_id),
                    ),
                    InputRejection::Busy => ("busy", SessionControlError::Busy(session_id)),
                };
                tracing::warn!(
                    name: "submit_rejected",
                    reason,
                    has_text,
                    text_bytes,
                );
                return (Err(error), None);
            }
        }
        tracing::info!(
            name: "submit_accepted",
            has_text,
            text_bytes,
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
            let mut runtimes = self.runtimes.borrow_mut();
            let Some(runtime) = runtimes.get_mut(&session_id) else {
                tracing::warn!(
                    name: "control_rejected",
                    op = op_name,
                    reason = "session_closed",
                );
                let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
                return None;
            };
            match runtime.request_control(op) {
                ControlRequest::Closed => {
                    tracing::warn!(
                        name: "control_rejected",
                        op = op_name,
                        reason = "session_closed",
                    );
                    let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
                    return None;
                }
                ControlRequest::Idle => {
                    let _ = ack.try_send(Ok(()));
                    return None;
                }
                ControlRequest::Queued => runtime.queue_control_ack(ack),
            }
        }
        tracing::info!(name: "control_requested", op = op_name);
        self.ensure_session_drive(session_id)
    }
}
