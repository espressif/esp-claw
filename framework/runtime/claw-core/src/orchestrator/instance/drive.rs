use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use tracing::Instrument as _;

use crate::agent::AgentId;
use crate::event::{EventSink, SessionEvent};
use crate::orchestrator::control::{DriveControl, DriveStop};

use super::inflight::{tick_agent, ReadyAgent, TickedAgent};
use super::model::DriveOutput;
use super::OrchestratorInstance;

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Drive the root-visible foreground turn until the root is no longer ready
    /// or in flight. Background subagents may remain in flight after this
    /// returns; they stay on the instance and continue through
    /// [`drive_background_until_root_ready`](Self::drive_background_until_root_ready).
    pub(crate) async fn drive_root_turn(
        &mut self,
        control: &DriveControl,
        events: &EventSink,
    ) -> (DriveOutput, DriveStop) {
        let mut output = DriveOutput::default();
        let mut cancel_requested = false;
        let mut interrupt_requested = false;

        loop {
            if control.take_cancel() {
                cancel_requested = true;
            }
            if control.take_interrupt() {
                interrupt_requested = true;
            }
            let _ = control.take_wake();

            if !cancel_requested && !interrupt_requested {
                self.start_ready_agent_tasks(events);
            }

            if cancel_requested {
                if self.inflight.is_empty() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Cancelled);
                }
            } else if interrupt_requested {
                if !self.inflight.has_root() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Interrupted);
                }
            } else if !self.has_root_work() && !self.has_unprompted_approval() {
                break;
            }

            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            self.set_cancel_hook(control);

            let ticked = self.inflight.next_completed(control).await;
            self.route_ticked_agents(ticked, events);
        }
        control.clear_cancel_hook();
        output.absorb(self.take_next_approval_prompt());
        (output, DriveStop::Quiescent)
    }

    /// Poll background agents until they either make the root ready, run out of
    /// work, or are woken by a newer user command.
    pub(crate) async fn drive_background_until_root_ready(
        &mut self,
        control: &DriveControl,
        events: &EventSink,
    ) -> DriveStop {
        loop {
            if control.take_cancel() {
                control.clear_cancel_hook();
                return DriveStop::Cancelled;
            }
            if control.take_interrupt() {
                control.clear_cancel_hook();
                return DriveStop::Interrupted;
            }
            if control.take_wake() {
                control.clear_cancel_hook();
                return DriveStop::Interrupted;
            }
            if self.has_root_work() || self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return DriveStop::Quiescent;
            }

            self.start_ready_agent_tasks(events);
            if self.has_root_work() || self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return DriveStop::Quiescent;
            }

            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            self.set_cancel_hook(control);

            let ticked = self.inflight.next_completed(control).await;
            self.route_ticked_agents(ticked, events);
            if self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return DriveStop::Quiescent;
            }
        }
        control.clear_cancel_hook();
        DriveStop::Quiescent
    }

    /// Drive cancellation cleanup until no queued or in-flight work remains.
    pub(crate) async fn drive_cancelled(
        &mut self,
        control: &DriveControl,
        events: &EventSink,
    ) -> (DriveOutput, DriveStop) {
        let output = DriveOutput::default();
        loop {
            let _ = control.take_cancel();
            let _ = control.take_interrupt();
            let _ = control.take_wake();
            self.start_ready_agent_tasks(events);
            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }
            self.set_cancel_hook(control);
            let ticked = self.inflight.next_completed(control).await;
            self.route_ticked_agents(ticked, events);
        }
        control.clear_cancel_hook();
        (output, DriveStop::Cancelled)
    }

    fn set_cancel_hook(&self, control: &DriveControl) {
        let mut abort_handles = self.state.get().registry.abort_handles();
        abort_handles.extend(self.inflight.abort_handles());
        control.set_cancel_hook(move || {
            for handle in &abort_handles {
                handle.abort();
            }
        });
    }

    /// Start every currently-ready agent and retain its future in the session's
    /// in-flight table.
    fn start_ready_agent_tasks(&mut self, events: &EventSink) {
        self.flush_subagent_result_mailbox();
        let ready = self.drain_ready_agents();
        if ready.is_empty() {
            return;
        }
        self.refresh_snapshots();

        for ready in ready {
            let id = ready.id;
            let is_root = ready.is_root;
            let abort = ready.agent.abort_handle();
            let sink = if ready.is_root {
                events.clone()
            } else {
                EventSink::disabled()
            };
            let Some(meta) = self.state.get().meta.get(&id) else {
                continue;
            };
            let span = tracing::info_span!(
                "agent",
                run.agent = %id,
                kind = %meta.kind.as_str(),
                depth = meta.depth as u64,
            );
            self.inflight.spawn(
                id,
                is_root,
                abort,
                Box::pin(tick_agent(ready, sink).instrument(span)),
            );
        }
    }

    /// Reinsert completed agents, apply effects, then route completed outcomes.
    fn route_ticked_agents(&mut self, ticked: Vec<TickedAgent>, events: &EventSink) {
        let mut outcomes = Vec::with_capacity(ticked.len());
        for TickedAgent { id, agent, outcome } in ticked {
            if self.state.get().meta.contains_key(&id) {
                self.state.get_mut().registry.insert(id, agent);
                outcomes.push((id, outcome));
            }
        }

        self.apply_effects();
        for (id, outcome) in outcomes {
            if self.state.get().meta.contains_key(&id) {
                let output = self.route_outcome(id, outcome);
                for reply in output.replies {
                    // Plain answers already streamed their Output fragments.
                    if !reply.streamed {
                        events.emit(SessionEvent::Output { text: reply.text });
                    }
                }
            }
        }
        self.inflight.retain_live(&self.state.get().meta);
        self.flush_subagent_result_mailbox();
    }

    fn drain_ready_agents(&mut self) -> Vec<ReadyAgent> {
        let mut ready_agents = Vec::new();
        while let Some(id) = self.pop_ready() {
            if !self.state.get().meta.contains_key(&id) {
                continue;
            }
            let Some(agent) = self.state.get_mut().registry.take(id) else {
                continue;
            };
            ready_agents.push(ReadyAgent {
                id,
                is_root: self.state.get().root == Some(id),
                agent,
            });
        }
        ready_agents
    }

    fn pop_ready(&mut self) -> Option<AgentId> {
        if self.state.get().ready.is_empty() {
            return None;
        }
        self.state.get_mut().ready.pop_front()
    }
}
