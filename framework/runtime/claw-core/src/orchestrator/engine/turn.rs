use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use tracing::Instrument as _;

use crate::event::{EventSink, SessionEvent};
use crate::orchestrator::ReasoningEffort;
use crate::session::{Message, SessionId, SessionPersistence};

use super::super::approval::{self, ApprovalResolverError, PermissionReplyResolution};
use super::super::control::{DriveControl, DriveStop};
use super::super::error::DeliverError;
use super::super::instance::{DriveOutput, OrchestratorInstance, PendingApproval, TurnStopMode};
use super::Engine;

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) fn announce_active_turn(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let turn = {
            let mut runtimes = self.runtimes.borrow_mut();
            let Some(runtime) = runtimes.get_mut(&session_id) else {
                return;
            };
            let Some(turn) = runtime.take_unannounced_turn() else {
                return;
            };
            turn
        };
        events.emit(SessionEvent::TurnStarted { turn });
    }

    pub(super) async fn drive_user_turn(&self, session_id: SessionId, input: Message) -> DriveStop {
        let Some(events) = self.session_events(session_id) else {
            return DriveStop::Quiescent;
        };
        let Some(persistence) = self.sessions.persistence(session_id) else {
            return DriveStop::Quiescent;
        };
        let Some((turn, effort)) = self
            .runtimes
            .borrow()
            .get(&session_id)
            .and_then(|runtime| runtime.active_turn_context())
        else {
            return DriveStop::Quiescent;
        };
        let has_text = !input.as_str().is_empty();
        let text_bytes = input.as_str().len() as u64;
        let stop = async {
            self.set_foreground_active(session_id, true);
            tracing::info!(
                name: "input_delivered",
                has_text,
                text_bytes,
            );
            let result = self
                .drive_one_input(session_id, input.into_text(), effort, persistence, &events)
                .await;
            let stop = self.handle_drive_result(&events, result);
            self.set_foreground_active(session_id, false);
            stop
        }
        .instrument(tracing::info_span!("turn", run.turn = %turn, cause = "user_submit"))
        .await;
        stop
    }

    pub(super) async fn drive_root_ready_for_active_turn(
        &self,
        session_id: SessionId,
    ) -> DriveStop {
        let Some(events) = self.session_events(session_id) else {
            return DriveStop::Quiescent;
        };
        let Some((turn, effort)) = self
            .runtimes
            .borrow()
            .get(&session_id)
            .and_then(|runtime| runtime.active_turn_context())
        else {
            return DriveStop::Quiescent;
        };
        async {
            self.set_foreground_active(session_id, true);
            tracing::info!(name: "background_result", "");
            let result = self.drive_root_ready(session_id, effort, &events).await;
            let stop = self.handle_drive_result(&events, result);
            self.set_foreground_active(session_id, false);
            stop
        }
        .instrument(tracing::info_span!("turn", run.turn = %turn, cause = "background_result"))
        .await
    }

    pub(super) async fn drive_background(&self, session_id: SessionId) -> DriveStop {
        let Some(events) = self.session_events(session_id) else {
            return DriveStop::Quiescent;
        };
        let Some(mut slot) = self.checkout_existing_instance(session_id) else {
            return DriveStop::Quiescent;
        };
        let control = DriveControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let (output, stop) = slot
            .get_mut()
            .drive_background_until_root_ready(&control, &events)
            .await;
        self.set_active_control(session_id, None);
        if stop != DriveStop::Quiescent {
            let mode = if stop == DriveStop::Cancelled {
                TurnStopMode::DeleteSpawnedAgents
            } else {
                TurnStopMode::PreserveAgents
            };
            slot.get_mut().stop_turn_tasks(mode).await;
        }
        self.handle_drive_result(&events, Ok((output, stop)))
    }

    pub(super) fn finish_active_turn(&self, session_id: SessionId) {
        let Some(events) = self.session_events(session_id) else {
            return;
        };
        let (turn, control_acks) = {
            let mut runtimes = self.runtimes.borrow_mut();
            let Some(runtime) = runtimes.get_mut(&session_id) else {
                return;
            };
            runtime.finish_turn()
        };
        let Some(turn) = turn else {
            return;
        };
        if let Err(error) = self.checkpoint_session_runtime(session_id) {
            tracing::error!(name: "checkpoint_failed", target = "session_runtime", error = %error);
            events.emit(SessionEvent::Error {
                message: error.to_string(),
            });
        }
        events.emit(SessionEvent::TurnEnded { turn });
        for ack in control_acks {
            let _ = ack.try_send(Ok(()));
        }
    }

    fn handle_drive_result(
        &self,
        events: &EventSink,
        result: Result<(DriveOutput, DriveStop), DeliverError>,
    ) -> DriveStop {
        match result {
            Ok((output, stop)) => {
                for text in output.into_messages() {
                    tracing::info!(name: "output", text_bytes = text.len() as u64);
                    events.emit(SessionEvent::Output { text });
                }
                stop
            }
            Err(error) => {
                let kind: &'static str = (&error).into();
                tracing::error!(name: "error", kind);
                events.emit(SessionEvent::Error {
                    message: error.to_string(),
                });
                DriveStop::Quiescent
            }
        }
    }

    async fn drive_one_input(
        &self,
        session_id: SessionId,
        text: String,
        effort: ReasoningEffort,
        persistence: SessionPersistence,
        events: &EventSink,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();

        if let Some(pending) = instance.active_approval() {
            instance.set_root_context_block(effort.context_block())?;
            let control = DriveControl::new();
            self.set_active_control(session_id, Some(control.clone()));
            let result = self
                .resolve_pending_approval(instance, pending, &text, &control, events)
                .await;
            self.set_active_control(session_id, None);
            return result;
        }

        instance.deliver(text, effort.context_block(), persistence)?;
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
        let (output, stop) = instance.drive_root_turn(&control, events).await;
        self.set_active_control(session_id, None);
        if stop != DriveStop::Quiescent {
            let mode = if stop == DriveStop::Cancelled {
                TurnStopMode::DeleteSpawnedAgents
            } else {
                TurnStopMode::PreserveAgents
            };
            instance.stop_turn_tasks(mode).await;
            tracing::warn!(name: "turn_stopped", mode = ?mode);
        }
        Ok((output, stop))
    }

    async fn resolve_pending_approval(
        &self,
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
                instance
                    .stop_turn_tasks(TurnStopMode::DeleteSpawnedAgents)
                    .await;
                return Ok((DriveOutput::default(), DriveStop::Cancelled));
            }
            Err(error) => return Err(error.into()),
        };

        let Some(decision) = resolution.clone().into_decision() else {
            let PermissionReplyResolution::Clarify(message) = resolution else {
                unreachable!("non-clarification resolutions map to approval decisions")
            };
            tracing::info!(name: "approval_clarification", reason = "clarify");
            return Ok((DriveOutput::message(message), DriveStop::Quiescent));
        };

        let decision_name: &'static str = (&decision).into();
        tracing::info!(name: "approval_resolved", decision = decision_name);
        instance
            .resolve_active_approval(decision)
            .map_err(DeliverError::from)?;
        Ok(instance.drive_root_turn(control, events).await)
    }
}
