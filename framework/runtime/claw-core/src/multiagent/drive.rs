use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use tracing::Instrument as _;

use crate::config::ApiUsage;
use crate::protocol::{AgentId, EventSink, TurnOrigin};

use super::agents::{tick_agent, CompletedAgentTick, ReadyAgent};
use super::drive_control::{DriveControl, DriveStop};
use super::{MultiagentRuntime, MultiagentWork};

/// Messages created outside the LLM stream and still awaiting engine emission.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct DriveOutput {
    messages: Vec<String>,
}

impl DriveOutput {
    fn absorb(&mut self, other: DriveOutput) {
        self.messages.extend(other.messages);
    }

    pub(crate) fn message(message: String) -> Self {
        Self {
            messages: vec![message],
        }
    }

    pub(crate) fn into_messages(self) -> Vec<String> {
        self.messages
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TurnStopMode {
    PreserveAgents,
    DeleteSpawnedAgents,
}

impl<Filesystem, Http, Timer> MultiagentRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::multiagent) fn clear_turn_work(&mut self) {
        self.state.get_mut().clear_turn_work();
        self.slots.clear_inboxes();
        self.foreground_results.clear();
        self.multiagent.clear();
    }

    pub(crate) fn work(&self) -> MultiagentWork {
        let root = self.state.get().root();
        let scheduled = self.state.get().work(
            self.slots.has_running_root(),
            self.slots.has_running_background(),
        );
        if scheduled != MultiagentWork::None {
            return scheduled;
        }
        let Some(root) = root else {
            return MultiagentWork::None;
        };
        if self.slots.has_inbox(root) {
            MultiagentWork::Root
        } else if self.slots.has_inbox_except(root) {
            MultiagentWork::Background
        } else {
            MultiagentWork::None
        }
    }

    pub(crate) fn pending_root_origin(&self) -> Option<TurnOrigin> {
        let root = self.state.get().root()?;
        self.slots.first_inbox_origin(root)
    }

    pub(crate) fn activate_pending_root_result(&mut self) -> bool {
        let Some(root) = self.state.get().root() else {
            return false;
        };
        if !self.slots.activate_next_message(root) {
            return false;
        }
        self.enqueue(root);
        true
    }

    pub(in crate::multiagent) fn enqueue(&mut self, id: AgentId) {
        self.state.get_mut().enqueue(id);
    }

    pub(in crate::multiagent) fn has_ready(&self) -> bool {
        self.state.get().has_ready()
    }

    pub(in crate::multiagent) fn has_root_work(&self) -> bool {
        let Some(root) = self.state.get().root() else {
            return false;
        };
        self.state.get().is_ready(root) || self.slots.has_running_root()
    }

    /// Stop every task owned by the active turn. Agents are first recovered from
    /// in-flight futures, then reset to idle through their normal cancel reducer.
    /// The caller chooses whether the durable graph is preserved or pruned.
    pub(crate) async fn stop_turn_tasks(&mut self, mode: TurnStopMode) {
        let events = EventSink::disabled();
        let control = DriveControl::new();

        self.cancel_foreground_results();
        self.slots.abort_all();
        while self.slots.has_running() {
            let _ = self.slots.next_completed(&control).await;
        }

        self.clear_turn_work();
        self.cancel_all();
        while self.has_ready() || self.slots.has_running() {
            self.start_ready_agent_tasks(&events, true);
            if !self.slots.has_running() {
                continue;
            }
            let _ = self.slots.next_completed(&control).await;
        }
        self.clear_turn_work();
        if mode == TurnStopMode::DeleteSpawnedAgents {
            self.delete_spawned_subagents();
        }
        self.refresh_multiagent_snapshot();
    }

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
                self.cancel_foreground_results();
            }
            if control.take_interrupt() {
                interrupt_requested = true;
            }
            let _ = control.take_wake();

            if !cancel_requested && !interrupt_requested {
                self.start_ready_agent_tasks(events, false);
            }

            if cancel_requested {
                if !self.slots.has_running() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Cancelled);
                }
            } else if interrupt_requested {
                if !self.slots.has_running_root() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Interrupted);
                }
            } else if !self.has_root_work() && !self.has_unprompted_approval() {
                break;
            }

            if !self.slots.has_running() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            self.set_cancel_hook(control);

            let completed = self
                .slots
                .next_completed_or_command(control, &self.multiagent)
                .await;
            output.absorb(self.route_completed_agents(completed));
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
    ) -> (DriveOutput, DriveStop) {
        let mut output = DriveOutput::default();
        loop {
            if control.take_cancel() {
                control.clear_cancel_hook();
                return (output, DriveStop::Cancelled);
            }
            if control.take_interrupt() {
                control.clear_cancel_hook();
                return (output, DriveStop::Interrupted);
            }
            if control.take_wake() {
                control.clear_cancel_hook();
                return (output, DriveStop::Woken);
            }
            if self.pending_root_origin().is_some()
                || self.has_root_work()
                || self.has_unprompted_approval()
            {
                control.clear_cancel_hook();
                return (output, DriveStop::Quiescent);
            }

            self.start_ready_agent_tasks(events, false);
            if self.pending_root_origin().is_some()
                || self.has_root_work()
                || self.has_unprompted_approval()
            {
                control.clear_cancel_hook();
                return (output, DriveStop::Quiescent);
            }

            if !self.slots.has_running() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            self.set_cancel_hook(control);

            let completed = self
                .slots
                .next_completed_or_command(control, &self.multiagent)
                .await;
            output.absorb(self.route_completed_agents(completed));
            if self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return (output, DriveStop::Quiescent);
            }
        }
        control.clear_cancel_hook();
        (output, DriveStop::Quiescent)
    }

    fn set_cancel_hook(&self, control: &DriveControl) {
        let abort_handles = self.slots.abort_handles();
        control.set_cancel_hook(move || {
            for handle in &abort_handles {
                handle.abort();
            }
        });
    }

    /// Start every currently-ready agent in its stable slot.
    fn start_ready_agent_tasks(&mut self, events: &EventSink, include_root_inbox: bool) {
        self.schedule_pending_inboxes(include_root_inbox);
        let ready = self.drain_ready_agents();
        if ready.is_empty() {
            return;
        }

        for mut ready in ready {
            let id = ready.id;
            let is_root = ready.is_root;
            // Snapshot this turn's config for the agent's usage (root vs sub),
            // resolved from the shared manager. A turn thus runs on one config
            // even if it is updated mid-turn. `None` (nothing linked) or an
            // invalid config leaves the agent on its current client.
            let usage = if is_root {
                ApiUsage::RootAgent
            } else {
                ApiUsage::SubAgent
            };
            if let Some(config) = self.factory.config_for(usage) {
                if ready.agent.set_llm_config(config).is_err() {
                    tracing::error!(name: "llm_config_invalid", agent = %id);
                }
            }
            let abort = ready.agent.abort_handle();
            let sink = if ready.is_root {
                events.clone()
            } else {
                EventSink::disabled()
            };
            let meta = self
                .state
                .get()
                .node(id)
                .expect("a ready agent must remain in the live graph");
            let span = tracing::info_span!(
                "agent",
                run.agent = %id,
                kind = %meta.kind().as_str(),
                depth = self
                    .state
                    .get()
                    .depth(id)
                    .expect("live graph topology is valid") as u64,
            );
            self.slots.start(
                id,
                is_root,
                abort,
                Box::pin(tick_agent(ready, sink).instrument(span)),
            );
        }
        self.refresh_multiagent_snapshot();
    }

    /// Apply subagent commands, then route outcomes from agents already restored
    /// to their stable slots.
    fn route_completed_agents(&mut self, completed: Vec<CompletedAgentTick>) -> DriveOutput {
        let mut output = DriveOutput::default();
        self.apply_multiagent_commands();
        for CompletedAgentTick { id, outcome } in completed {
            if self.state.get().contains(id) {
                output.absorb(self.route_outcome(id, outcome));
            }
        }
        self.refresh_multiagent_snapshot();
        output
    }

    fn drain_ready_agents(&mut self) -> Vec<ReadyAgent<Http, Timer>> {
        let mut ready_agents = Vec::new();
        while let Some(id) = self.pop_ready() {
            if !self.state.get().contains(id) {
                continue;
            }
            let Some(agent) = self.slots.take_idle(id) else {
                continue;
            };
            ready_agents.push(ReadyAgent {
                id,
                is_root: self.state.get().is_root(id),
                agent,
            });
        }
        ready_agents
    }

    fn schedule_pending_inboxes(&mut self, include_root: bool) {
        let root = self.state.get().root();
        let pending = self.slots.ready_inbox_ids().collect::<Vec<_>>();
        for id in pending {
            if (include_root || Some(id) != root)
                && self.state.get().contains(id)
                && !self.state.get().is_awaiting_approval(id)
                && self.slots.activate_next_message(id)
            {
                self.enqueue(id);
            }
        }
    }

    fn pop_ready(&mut self) -> Option<AgentId> {
        self.state.get_mut().pop_ready()
    }
}

#[cfg(test)]
mod tests {
    use super::DriveOutput;

    #[test]
    fn drive_output_owns_only_messages_that_still_need_emission() {
        let mut output = DriveOutput::message("first".to_owned());
        output.absorb(DriveOutput::message("second".to_owned()));

        assert_eq!(
            output.into_messages(),
            vec!["first".to_owned(), "second".to_owned()]
        );
    }
}
