use std::rc::Rc;

use async_channel::Sender;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::event::EventSink;
use crate::orchestrator::{OpenSessionError, ReasoningEffort, SessionControlError};
use crate::session::SessionId;

use super::super::{DriveFuture, Engine, InstanceWork, SessionRuntime};

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::engine::command_loop) fn open_session_stream(
        self: &Rc<Self>,
        session_id: SessionId,
        events: EventSink,
    ) -> (Result<(), OpenSessionError>, Option<DriveFuture>) {
        if !self.sessions.contains(session_id) {
            return (Err(OpenSessionError::SessionNotFound(session_id)), None);
        }
        let has_pending_input = {
            let mut runtimes = self.runtimes.borrow_mut();
            let runtime = runtimes
                .entry(session_id)
                .or_insert_with(SessionRuntime::fresh);
            let Some(has_pending_input) = runtime.open(events) else {
                return (Err(OpenSessionError::AlreadyOpen(session_id)), None);
            };
            has_pending_input
        };
        let should_start =
            has_pending_input || !matches!(self.instance_work(session_id), InstanceWork::None);
        let future = should_start
            .then(|| self.ensure_session_drive(session_id))
            .flatten();
        (Ok(()), future)
    }

    /// Store a new reasoning effort on the session's drive. Config-only: it
    /// updates the pending value (applied at the next turn) and never starts or
    /// disturbs a drive, so there is no follow-up future.
    pub(in crate::orchestrator::engine::command_loop) fn set_reasoning_effort(
        self: &Rc<Self>,
        session_id: SessionId,
        effort: ReasoningEffort,
    ) -> Result<(), SessionControlError> {
        let mut runtimes = self.runtimes.borrow_mut();
        let Some(runtime) = runtimes.get_mut(&session_id) else {
            return Err(SessionControlError::SessionClosed(session_id));
        };
        runtime.set_reasoning_effort(effort);
        Ok(())
    }

    pub(in crate::orchestrator::engine::command_loop) fn close_session(
        self: &Rc<Self>,
        session_id: SessionId,
        ack: Sender<Result<(), SessionControlError>>,
    ) -> Option<DriveFuture> {
        let session_span = tracing::info_span!("session", run.session = %session_id);
        let _session_enter = session_span.enter();
        if !self.sessions.contains(session_id) {
            tracing::warn!(name: "close_rejected", reason = "session_closed");
            let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
            return None;
        }
        {
            let mut runtimes = self.runtimes.borrow_mut();
            let Some(runtime) = runtimes.get_mut(&session_id) else {
                tracing::warn!(name: "close_rejected", reason = "not_open");
                let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
                return None;
            };
            if !runtime.is_open() && !runtime.is_closing() {
                tracing::warn!(name: "close_rejected", reason = "not_open");
                let _ = ack.try_send(Err(SessionControlError::SessionClosed(session_id)));
                return None;
            }
            runtime.queue_close_ack(ack);
            if runtime.is_closing() {
                tracing::info!(name: "close_requested", "");
                return None;
            }
            tracing::info!(name: "close_requested", "");
        }
        self.session_shutdown(session_id)
    }

    pub(in crate::orchestrator::engine::command_loop) fn delete_session(
        self: &Rc<Self>,
        session_id: SessionId,
    ) -> (Result<(), SessionControlError>, Option<DriveFuture>) {
        let session_span = tracing::info_span!("session", run.session = %session_id);
        let _session_enter = session_span.enter();
        let delete_span = tracing::info_span!("session.delete", run.session = %session_id);
        let existed = delete_span.in_scope(|| {
            let existed = self.sessions.delete(session_id);
            if existed {
                tracing::info!(name: "delete_requested", "");
                tracing::info!(name: "registry_removed", "");
            } else {
                tracing::warn!(name: "delete_rejected", reason = "session_closed");
            }
            existed
        });
        if !existed {
            return (Err(SessionControlError::SessionClosed(session_id)), None);
        }
        {
            let runtimes = self.runtimes.borrow();
            if !runtimes.contains_key(&session_id) {
                drop(runtimes);
                tracing::info_span!("session.delete", run.session = %session_id).in_scope(|| {
                    tracing::info!(name: "runtime_state_removed", "");
                });
                return (Ok(()), None);
            };
        }
        (Ok(()), self.session_shutdown(session_id))
    }
}
