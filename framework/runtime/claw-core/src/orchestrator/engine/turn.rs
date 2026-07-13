use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use tracing::Instrument as _;

use crate::agent::CancelReason;
use crate::event::{EventSink, SessionEvent, TurnCause};
use crate::orchestrator::ReasoningEffort;
use crate::session::{SessionId, TurnId};

use super::super::approval::{self, ApprovalResolverError, PermissionReplyResolution};
use super::super::control::{DriveControl, DriveStop};
use super::super::error::DeliverError;
use super::super::instance::{DriveOutput, OrchestratorInstance, PendingApproval, RootReply};
use super::session_drive::SubmittedInput;
use super::Engine;

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) async fn drive_user_turn(&self, session_id: SessionId, input: SubmittedInput) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let Some((turn, effort)) = self.drives.borrow_mut().get_mut(&session_id).map(|drive| {
            let turn = drive.next_turn();
            (turn, drive.reasoning_effort())
        }) else {
            return;
        };
        let has_text = !input.text.is_empty();
        let text_bytes = input.text.len() as u64;
        async {
            self.set_foreground_active(session_id, true);
            events.emit(SessionEvent::TurnStarted {
                turn,
                cause: TurnCause::UserSubmit,
            });
            tracing::info!(
                name: "input_delivered",
                has_text,
                text_bytes,
                attachment_count = 0_u64,
                attachment_kinds = "none",
            );
            let result = self
                .drive_one_input(session_id, input.text, effort, &events)
                .await;
            self.finish_turn(session_id, turn, &events, result);
            self.set_foreground_active(session_id, false);
        }
        .instrument(tracing::info_span!("turn", run.turn = %turn, cause = "user_submit"))
        .await;
    }

    pub(super) async fn drive_background_result_turn(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let Some((turn, effort)) = self.drives.borrow_mut().get_mut(&session_id).map(|drive| {
            let turn = drive.next_turn();
            (turn, drive.reasoning_effort())
        }) else {
            return;
        };
        async {
            self.set_foreground_active(session_id, true);
            events.emit(SessionEvent::TurnStarted {
                turn,
                cause: TurnCause::BackgroundResult,
            });
            tracing::info!(name: "background_result", "");
            let result = self.drive_root_ready(session_id, effort, &events).await;
            self.finish_turn(session_id, turn, &events, result);
            self.set_foreground_active(session_id, false);
        }
        .instrument(tracing::info_span!(
            "turn",
            run.turn = %turn,
            cause = "background_result"
        ))
        .await;
    }

    pub(super) async fn drive_background(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let Some(mut slot) = self.checkout_existing_instance(session_id) else {
            return;
        };
        let control = DriveControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let stop = slot
            .get_mut()
            .drive_background_until_root_ready(&control, &events)
            .await;
        self.set_active_control(session_id, None);
        if stop == DriveStop::Cancelled {
            slot.get_mut().cancel_all(CancelReason::UserRequested);
            let cleanup_control = DriveControl::new();
            cleanup_control.request_cancel();
            let _ = slot
                .get_mut()
                .drive_cancelled(&cleanup_control, &events)
                .await;
        }
    }

    fn finish_turn(
        &self,
        session_id: SessionId,
        turn: TurnId,
        events: &EventSink,
        result: Result<(DriveOutput, DriveStop), DeliverError>,
    ) {
        match result {
            Ok((output, _stop)) => {
                for reply in output.replies {
                    tracing::info!(name: "output", text_bytes = reply.text.len() as u64);
                    // Plain answers already streamed their Output fragments.
                    if !reply.streamed {
                        events.emit(SessionEvent::Output { text: reply.text });
                    }
                }
            }
            Err(error) => {
                let kind: &'static str = (&error).into();
                tracing::error!(name: "error", kind);
                events.emit(SessionEvent::Error {
                    message: error.to_string(),
                });
            }
        }
        if let Err(error) = self.checkpoint_session_runtime(session_id) {
            tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
            events.emit(SessionEvent::Error {
                message: error.to_string(),
            });
        }
        events.emit(SessionEvent::TurnEnded { turn });
    }

    async fn drive_one_input(
        &self,
        session_id: SessionId,
        text: String,
        effort: ReasoningEffort,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();

        if let Some(pending) = instance.active_approval() {
            instance.set_root_context_block(effort.context_block())?;
            let control = DriveControl::new();
            self.set_active_control(session_id, Some(control.clone()));
            let result = self
                .resolve_pending_approval(session_id, instance, pending, &text, &control, events)
                .await;
            self.set_active_control(session_id, None);
            return result;
        }

        instance.deliver(text, effort.context_block())?;
        self.drive_root_ready_in_slot(session_id, instance, events)
            .await
    }

    async fn drive_root_ready(
        &self,
        session_id: SessionId,
        effort: ReasoningEffort,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();
        instance.set_root_context_block(effort.context_block())?;
        self.drive_root_ready_in_slot(session_id, instance, events)
            .await
    }

    async fn drive_root_ready_in_slot(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<Filesystem, Http, Timer>,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let control = DriveControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let (mut output, stop) = instance.drive_root_turn(&control, events).await;
        self.set_active_control(session_id, None);
        if stop == DriveStop::Cancelled {
            instance.cancel_all(CancelReason::UserRequested);
            let cleanup_control = DriveControl::new();
            cleanup_control.request_cancel();
            let (cleanup_output, _) = instance.drive_cancelled(&cleanup_control, events).await;
            output.absorb(cleanup_output);
            tracing::warn!(name: "cancelled_cleanup", "");
        }
        Ok((output, stop))
    }

    async fn resolve_pending_approval(
        &self,
        session_id: SessionId,
        instance: &mut OrchestratorInstance<Filesystem, Http, Timer>,
        pending: PendingApproval,
        user_reply: &str,
        control: &DriveControl,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let resolution = match approval::resolve_permission_reply::<Http, Timer>(
            &self.api_manager,
            &pending.summary,
            user_reply,
            control,
        )
        .await
        {
            Ok(resolution) => resolution,
            Err(ApprovalResolverError::Cancelled) => {
                instance.cancel_all(CancelReason::UserRequested);
                let cleanup_control = DriveControl::new();
                cleanup_control.request_cancel();
                let (cleanup_output, _) = instance.drive_cancelled(&cleanup_control, events).await;
                return Ok((cleanup_output, DriveStop::Cancelled));
            }
            Err(error) => return Err(error.into()),
        };

        let Some(decision) = resolution.clone().into_decision() else {
            let PermissionReplyResolution::Clarify(message) = resolution else {
                unreachable!("non-clarification resolutions map to approval decisions")
            };
            tracing::info!(name: "approval_clarification", reason = "clarify");
            return Ok((
                DriveOutput {
                    replies: vec![RootReply {
                        session: session_id,
                        text: message,
                        streamed: false,
                    }],
                },
                DriveStop::Quiescent,
            ));
        };

        let decision_name: &'static str = (&decision).into();
        tracing::info!(name: "approval_resolved", decision = decision_name);
        instance
            .resolve_active_approval(decision)
            .map_err(DeliverError::from)?;
        Ok(instance.drive_root_turn(control, events).await)
    }
}
