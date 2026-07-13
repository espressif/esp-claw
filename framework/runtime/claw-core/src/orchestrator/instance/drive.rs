use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use tracing::Instrument as _;

use crate::agent::AgentId;
use crate::config::ApiUsage;
use crate::event::EventSink;
use crate::orchestrator::control::{DriveControl, DriveStop};

use super::inflight::{tick_agent, ReadyAgent, TickedAgent};
use super::{InstanceWork, OrchestratorInstance};

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

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::instance) fn clear_turn_work(&mut self) {
        self.state.get_mut().scheduler.clear_turn_work();
        self.effects
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .clear();
    }

    pub(crate) fn work(&self) -> InstanceWork {
        self.state.get().scheduler.work(
            self.state.get().graph.root(),
            self.inflight.has_root(),
            self.inflight.has_background(),
        )
    }

    pub(in crate::orchestrator::instance) fn enqueue(&mut self, id: AgentId) {
        self.state.get_mut().scheduler.enqueue(id);
    }

    pub(in crate::orchestrator::instance) fn has_ready(&self) -> bool {
        self.state.get().scheduler.has_ready()
    }

    pub(in crate::orchestrator::instance) fn has_root_work(&self) -> bool {
        let Some(root) = self.state.get().graph.root() else {
            return false;
        };
        self.state.get().scheduler.is_ready(root) || self.inflight.has_root()
    }

    /// Stop every task owned by the active turn. Agents are first recovered from
    /// in-flight futures, then reset to idle through their normal cancel reducer.
    /// The caller chooses whether the durable graph is preserved or pruned.
    pub(crate) async fn stop_turn_tasks(&mut self, mode: TurnStopMode) {
        let events = EventSink::disabled();
        let control = DriveControl::new();

        self.inflight.abort_all();
        while !self.inflight.is_empty() {
            let ticked = self.inflight.next_completed(&control).await;
            self.reinsert_stopped_agents(ticked);
        }

        self.clear_turn_work();
        self.cancel_all();
        while self.has_ready() || !self.inflight.is_empty() {
            self.start_ready_agent_tasks(&events);
            if self.inflight.is_empty() {
                continue;
            }
            let ticked = self.inflight.next_completed(&control).await;
            self.reinsert_stopped_agents(ticked);
        }
        self.clear_turn_work();
        if mode == TurnStopMode::DeleteSpawnedAgents {
            self.delete_spawned_subagents();
        }
        self.refresh_snapshots();
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
            output.absorb(self.route_ticked_agents(ticked));
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
                return (output, DriveStop::Interrupted);
            }
            if self.has_root_work() || self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return (output, DriveStop::Quiescent);
            }

            self.start_ready_agent_tasks(events);
            if self.has_root_work() || self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return (output, DriveStop::Quiescent);
            }

            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            self.set_cancel_hook(control);

            let ticked = self.inflight.next_completed(control).await;
            output.absorb(self.route_ticked_agents(ticked));
            if self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return (output, DriveStop::Quiescent);
            }
        }
        control.clear_cancel_hook();
        (output, DriveStop::Quiescent)
    }

    fn set_cancel_hook(&self, control: &DriveControl) {
        let mut abort_handles = self.registry.abort_handles();
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
            let Some(meta) = self.state.get().graph.node(id) else {
                continue;
            };
            let span = tracing::info_span!(
                "agent",
                run.agent = %id,
                kind = %meta.kind().as_str(),
                depth = self
                    .state
                    .get()
                    .graph
                    .depth(id)
                    .expect("live graph topology is valid") as u64,
            );
            self.inflight.spawn(
                id,
                is_root,
                abort,
                Box::pin(tick_agent(ready, sink).instrument(span)),
            );
        }
        self.refresh_snapshots();
    }

    /// Reinsert completed agents, apply effects, then route completed outcomes.
    fn route_ticked_agents(&mut self, ticked: Vec<TickedAgent<Http, Timer>>) -> DriveOutput {
        let mut outcomes = Vec::with_capacity(ticked.len());
        let mut output = DriveOutput::default();
        for TickedAgent { id, agent, outcome } in ticked {
            if self.state.get().graph.contains(id) {
                self.registry.insert(id, agent);
                outcomes.push((id, outcome));
            }
        }

        self.apply_effects();
        for (id, outcome) in outcomes {
            if self.state.get().graph.contains(id) {
                output.absorb(self.route_outcome(id, outcome));
            }
        }
        self.inflight.retain_live(&self.state.get().graph);
        self.flush_subagent_result_mailbox();
        self.refresh_snapshots();
        output
    }

    fn reinsert_stopped_agents(&mut self, ticked: Vec<TickedAgent<Http, Timer>>) {
        for TickedAgent { id, agent, .. } in ticked {
            if self.state.get().graph.contains(id) {
                self.registry.insert(id, agent);
            }
        }
        self.inflight.retain_live(&self.state.get().graph);
    }

    fn drain_ready_agents(&mut self) -> Vec<ReadyAgent<Http, Timer>> {
        let mut ready_agents = Vec::new();
        while let Some(id) = self.pop_ready() {
            if !self.state.get().graph.contains(id) {
                continue;
            }
            let Some(agent) = self.registry.take(id) else {
                continue;
            };
            ready_agents.push(ReadyAgent {
                id,
                is_root: self.state.get().graph.is_root(id),
                agent,
            });
        }
        ready_agents
    }

    fn pop_ready(&mut self) -> Option<AgentId> {
        self.state.get_mut().scheduler.pop_ready()
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
