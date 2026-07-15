use core::future::Future as _;
use core::pin::Pin;
use core::task::{Context, Poll};

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentCommand, TickOutcome};
use crate::protocol::{AgentId, Message};

use super::agent_control::AgentMessageDeliveryError;
use super::model::SubagentResult;
use super::timeouts::ExpiredTimeout;
use super::tool_port::{MultiagentAction, MultiagentCommand, SpawnCommand};
use super::{AgentPlacement, DriveOutput, MultiagentRuntime};

impl<Filesystem, Http, Timer> MultiagentRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Apply every subagent command emitted since the previous scheduling
    /// boundary. This is the single mutation entry point for model-facing
    /// spawn, followup, and delete operations.
    pub(in crate::multiagent) fn apply_multiagent_commands(&mut self) {
        for command in self.multiagent.drain() {
            let (requester, action) = command.into_parts();
            match action {
                MultiagentAction::Spawn(spawn) => self.spawn_subagent(requester, spawn),
                MultiagentAction::Delete { target } => self.delete_subagent(requester, target),
                MultiagentAction::Followup { target, message } => {
                    self.followup_subagent(requester, target, message)
                }
            }
        }
    }

    fn spawn_subagent(&mut self, parent: AgentId, spawn: SpawnCommand) {
        let (id, spec, completion) = spawn.into_parts();
        let (kind, name, goal, timeout) = spec.into_parts();
        if !self.state.get().contains(parent) {
            tracing::warn!(
                name: "spawn_dropped",
                parent_agent = %parent,
                kind = %kind.as_str(),
                reason = "missing_parent",
            );
            Self::send_spawn_failure(
                completion,
                id,
                "subagent parent no longer exists".to_owned(),
            );
            return;
        }

        match self.build_agent(id, &kind, goal, AgentPlacement::Child(id), Vec::new()) {
            Ok(()) => {
                let inserted =
                    self.state
                        .get_mut()
                        .insert_child(parent, id, kind.clone(), name, timeout);
                if !inserted {
                    self.slots.remove(id);
                    tracing::warn!(
                        name: "spawn_dropped",
                        parent_agent = %parent,
                        kind = %kind.as_str(),
                        reason = "missing_parent",
                    );
                    self.report_spawn_failure(
                        parent,
                        id,
                        completion,
                        "subagent parent no longer exists".to_owned(),
                    );
                    return;
                }
                if let Some(completion) = completion {
                    assert!(
                        self.foreground_results.insert(id, completion).is_none(),
                        "foreground result waiter already exists: {id}"
                    );
                }
                self.timeouts.arm::<Timer>(id, timeout);
                tracing::info!(
                    name: "spawn_materialized",
                    parent_agent = %parent,
                    child_agent = %id,
                    kind = %kind.as_str(),
                    timeout_ms = timeout.millis() as u64,
                );
                self.enqueue(id);
            }
            Err(error) => {
                let message = format!("failed to create subagent: {error}");
                tracing::error!(
                    name: "spawn_dropped",
                    parent_agent = %parent,
                    kind = %kind.as_str(),
                    reason = "build_failed",
                    error = %error,
                );
                self.report_spawn_failure(parent, id, completion, message);
            }
        }
    }

    fn report_spawn_failure(
        &mut self,
        parent: AgentId,
        child: AgentId,
        completion: Option<async_channel::Sender<SubagentResult>>,
        message: String,
    ) {
        if completion.is_some() {
            Self::send_spawn_failure(completion, child, message);
        } else {
            self.deliver_subagent_result(parent, child, message, false);
        }
    }

    fn send_spawn_failure(
        completion: Option<async_channel::Sender<SubagentResult>>,
        child: AgentId,
        message: String,
    ) {
        if let Some(completion) = completion {
            let _ = completion.try_send(SubagentResult::new(child, message, false));
        }
    }

    fn followup_subagent(&mut self, requester: AgentId, target: AgentId, message: Message) {
        if !self.state.get().is_strict_descendant(requester, target) {
            tracing::warn!(
                name: "followup_ignored",
                target_agent = %target,
                reason = "not_descendant",
            );
            return;
        }
        if self.slots.abort_if_running(target) {
            tracing::info!(
                name: "followup_deferred",
                target_agent = %target,
                reason = "running_abort",
            );
            self.multiagent.requeue(MultiagentCommand::new(
                requester,
                MultiagentAction::Followup { target, message },
            ));
            return;
        }
        if let Err(error) = self.deliver_followup(target, message) {
            tracing::warn!(
                name: "followup_ignored",
                target_agent = %target,
                reason = "delivery_failed",
                error = %error,
            );
        }
    }

    /// Followup is intentionally live-only: it cancels the target's current
    /// task and starts another task on the same in-memory agent.
    fn deliver_followup(
        &mut self,
        id: AgentId,
        message: Message,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.slots.available_agent_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        let _ = agent.send_command(AgentCommand::Cancel);
        agent.send_command(AgentCommand::AppendMessage(message))?;
        self.enqueue(id);
        tracing::info!(name: "followup_delivered", target_agent = %id);
        Ok(())
    }

    fn delete_subagent(&mut self, requester: AgentId, target: AgentId) {
        if !self.state.get().is_strict_descendant(requester, target) {
            tracing::warn!(
                name: "delete_ignored",
                target_agent = %target,
                reason = "not_descendant",
            );
            return;
        }
        self.delete_subtree(target);
    }

    fn delete_subtree(&mut self, root: AgentId) {
        let victims = self.state.get().subtree_ids(root);
        tracing::info!(
            name: "subtree_deleted",
            root_agent = %root,
            count = victims.len() as u64,
        );
        for victim in &victims {
            self.timeouts.remove(*victim);
            if let Some(completion) = self.foreground_results.remove(victim) {
                let _ = completion.try_send(SubagentResult::new(
                    *victim,
                    "foreground subagent was deleted before returning a result".to_owned(),
                    false,
                ));
            }
            self.slots.remove(*victim);
        }
        self.state.get_mut().remove_agents(&victims);
    }

    pub(in crate::multiagent) fn route_expired_timeouts(
        &mut self,
        mut expired: Vec<ExpiredTimeout>,
    ) -> DriveOutput {
        expired.sort_by_key(|expired| self.state.get().depth(expired.agent).unwrap_or(u16::MAX));
        for expired in expired {
            if !self.state.get().contains(expired.agent) {
                continue;
            }
            let Some(parent) = self.state.get().parent(expired.agent).flatten() else {
                continue;
            };
            let victim_count = self.state.get().subtree_ids(expired.agent).len();
            let message = format!(
                "subagent timed out after {} ms; deleted its subtree of {victim_count} agent(s)",
                expired.timeout.millis()
            );
            tracing::warn!(
                name: "subtree_timed_out",
                root_agent = %expired.agent,
                timeout_ms = expired.timeout.millis() as u64,
                count = victim_count as u64,
            );
            self.deliver_subagent_result(parent, expired.agent, message, false);
            self.delete_subtree(expired.agent);
        }
        DriveOutput::default()
    }

    /// Keep deadlines live while the session actor is idle, including while it
    /// is waiting for a permission reply or has no open client lease.
    pub(crate) fn poll_expired_timeouts(&mut self, context: &mut Context<'_>) -> Poll<DriveOutput> {
        if !self.timeouts.has_pending() {
            return Poll::Pending;
        }
        let expired = {
            let mut next = self.timeouts.next_expired();
            let Poll::Ready(expired) = Pin::new(&mut next).poll(context) else {
                return Poll::Pending;
            };
            expired
        };
        let output = self.route_expired_timeouts(expired);
        self.refresh_multiagent_snapshot();
        Poll::Ready(output)
    }

    pub(in crate::multiagent) fn delete_spawned_subagents(&mut self) {
        let children = self.state.get().root_children();
        for child in children {
            self.delete_subtree(child);
        }
    }

    pub(in crate::multiagent) fn cancel_foreground_results(&mut self) {
        for (child, completion) in std::mem::take(&mut self.foreground_results) {
            let _ = completion.try_send(SubagentResult::new(
                child,
                "foreground subagent was cancelled".to_owned(),
                false,
            ));
        }
    }

    pub(in crate::multiagent) fn route_outcome(
        &mut self,
        id: AgentId,
        outcome: TickOutcome,
    ) -> DriveOutput {
        match outcome {
            TickOutcome::Working => {
                self.enqueue(id);
                DriveOutput::default()
            }
            TickOutcome::Idle => DriveOutput::default(),
            TickOutcome::AwaitingApproval { summary } => self.park_approval(id, summary),
            TickOutcome::Yielded { text } => self.route_yielded(id, text),
            TickOutcome::Ended { final_message } => self.route_terminal(id, final_message, true),
            TickOutcome::Cancelled => self.route_cancelled(id),
            TickOutcome::Failed(error) => {
                self.route_terminal(id, format!("[failed: {error:?}]"), false)
            }
        }
    }

    fn deliver_subagent_result(&mut self, parent: AgentId, child: AgentId, text: String, ok: bool) {
        if !self.state.get().contains(parent) {
            return;
        }
        let result = SubagentResult::new(child, text, ok);
        if let Some(completion) = self.foreground_results.remove(&child) {
            let delivered = completion.try_send(result).is_ok();
            tracing::info!(
                name: "result_to_foreground_tool",
                parent_agent = %parent,
                child_agent = %child,
                delivered,
            );
            return;
        }
        let Some(parent_availability) = self.slots.deliver_child_result(parent, result) else {
            tracing::warn!(
                name: "result_to_parent_failed",
                parent_agent = %parent,
                child_agent = %child,
                reason = "missing_slot",
            );
            return;
        };
        let awaiting_approval = self.state.get().is_awaiting_approval(parent);
        tracing::info!(
            name: "result_to_parent",
            parent_agent = %parent,
            child_agent = %child,
            queued = true,
            parent_availability = ?parent_availability,
            awaiting_approval,
        );
    }

    fn route_yielded(&mut self, id: AgentId, text: String) -> DriveOutput {
        let Some(parent) = self.state.get().parent(id) else {
            return DriveOutput::default();
        };
        let Some(parent_id) = parent else {
            return DriveOutput::default();
        };
        if self.defer_success_for_owned_work(id) {
            return DriveOutput::default();
        }

        self.deliver_subagent_result(parent_id, id, text, true);
        self.delete_subtree(id);
        DriveOutput::default()
    }

    fn route_terminal(&mut self, id: AgentId, text: String, ok: bool) -> DriveOutput {
        let Some(parent) = self.state.get().parent(id) else {
            return DriveOutput::default();
        };
        let Some(parent_id) = parent else {
            return DriveOutput::message(text);
        };
        if ok && self.defer_success_for_owned_work(id) {
            return DriveOutput::default();
        }

        self.deliver_subagent_result(parent_id, id, text, ok);
        self.delete_subtree(id);
        DriveOutput::default()
    }

    fn defer_success_for_owned_work(&self, id: AgentId) -> bool {
        let has_live_children = self.state.get().has_children(id);
        let has_pending_child_results = self.slots.has_inbox(id);
        if !has_live_children && !has_pending_child_results {
            return false;
        }
        tracing::info!(
            name: "subagent_completion_deferred",
            agent = %id,
            live_descendants = self.state.get().subtree_ids(id).len().saturating_sub(1) as u64,
            pending_child_results = has_pending_child_results,
        );
        true
    }

    fn route_cancelled(&mut self, id: AgentId) -> DriveOutput {
        if self.state.get().parent(id).flatten().is_none() {
            return DriveOutput::default();
        }
        self.delete_subtree(id);
        DriveOutput::default()
    }
}

#[cfg(test)]
mod tests {
    use std::rc::Rc;
    use std::sync::{Arc, RwLock};

    use claw_interface::{ImmediateTimer, MemFs, RealHttp};
    use claw_permission::AllowAll;
    use claw_tool::ToolRegistry;

    use crate::agent::{FsAgentFactory, TickOutcome};
    use crate::config::ClawApiManager;
    use crate::protocol::{AgentId, AgentKind, Message, SessionId};

    use super::super::model::{SubagentTimeout, TranscriptText};
    use super::super::timeouts::ExpiredTimeout;
    use super::super::{
        AgentIdAllocator, AgentPlacement, MultiagentRuntime, MultiagentState, ROOT_AGENT_KIND,
    };

    type TestInstance = MultiagentRuntime<MemFs, RealHttp, ImmediateTimer>;

    fn timeout() -> SubagentTimeout {
        SubagentTimeout::from_millis(60_000).expect("non-zero timeout")
    }

    #[allow(clippy::arc_with_non_send_sync)]
    fn instance_with_root() -> (TestInstance, AgentId) {
        MemFs::new();
        let factory = FsAgentFactory::new(
            Arc::new(ToolRegistry::new()),
            "/output-test".to_owned(),
            Vec::new(),
            Arc::new(RwLock::new(ClawApiManager::new())),
        )
        .expect("test factory builds");
        let mut instance = MultiagentRuntime::new(
            SessionId::new(1),
            Rc::new(factory),
            AgentIdAllocator::new(),
            Arc::new(AllowAll),
            MultiagentState::default(),
        );
        let root = AgentId(1);
        assert!(instance
            .state
            .get_mut()
            .insert_root(root, AgentKind::from_static(ROOT_AGENT_KIND)));
        (instance, root)
    }

    #[test]
    fn only_root_terminal_results_request_engine_emission() {
        let (mut instance, root) = instance_with_root();

        let yielded = instance.route_outcome(
            root,
            TickOutcome::Yielded {
                text: "streamed".to_owned(),
            },
        );
        assert!(yielded.into_messages().is_empty());

        let ended = instance.route_outcome(
            root,
            TickOutcome::Ended {
                final_message: "finished".to_owned(),
            },
        );
        assert_eq!(ended.into_messages(), vec!["finished".to_owned()]);
    }

    #[test]
    fn successful_subagent_with_live_children_is_parked_instead_of_deleted() {
        let (mut instance, root) = instance_with_root();
        let parent = AgentId(2);
        let child = AgentId(3);
        assert!(instance.state.get_mut().insert_child(
            root,
            parent,
            AgentKind::from_static("worker"),
            Some("epsilon".to_owned()),
            timeout(),
        ));
        assert!(instance.state.get_mut().insert_child(
            parent,
            child,
            AgentKind::from_static("worker"),
            Some("nested".to_owned()),
            timeout(),
        ));

        let output = instance.route_outcome(
            parent,
            TickOutcome::Yielded {
                text: "children spawned; waiting for their results".to_owned(),
            },
        );

        assert!(output.into_messages().is_empty());
        assert!(instance.state.get().contains(parent));
        assert!(instance.state.get().contains(child));
        assert_eq!(instance.state.get().node_count(), 3);
    }

    #[test]
    fn parent_timeout_reports_one_failure_and_deletes_its_entire_subtree() {
        let (mut instance, root) = instance_with_root();
        // Deliberately give the descendant a lower id: timeout dominance is
        // topology-based, never allocator/order-based.
        let parent = AgentId(3);
        let child = AgentId(2);
        assert!(instance.state.get_mut().insert_child(
            root,
            parent,
            AgentKind::from_static("worker"),
            Some("epsilon".to_owned()),
            timeout(),
        ));
        assert!(instance.state.get_mut().insert_child(
            parent,
            child,
            AgentKind::from_static("worker"),
            Some("nested".to_owned()),
            timeout(),
        ));
        instance.timeouts.arm::<ImmediateTimer>(parent, timeout());
        instance.timeouts.arm::<ImmediateTimer>(child, timeout());
        let (completion, results) = async_channel::bounded(1);
        instance.foreground_results.insert(parent, completion);

        instance.route_expired_timeouts(vec![
            ExpiredTimeout {
                agent: child,
                timeout: timeout(),
            },
            ExpiredTimeout {
                agent: parent,
                timeout: timeout(),
            },
        ]);

        let result = results.try_recv().expect("timeout result delivered");
        assert!(!result.ok());
        assert!(result.text().contains("timed out after 60000 ms"));
        assert!(instance.state.get().contains(root));
        assert!(!instance.state.get().contains(parent));
        assert!(!instance.state.get().contains(child));
        assert!(!instance.timeouts.has_pending());
        assert!(results.try_recv().is_err(), "timeout must report only once");
    }

    #[test]
    fn early_child_result_parks_and_wakes_its_parent_before_the_parent_bubbles_up() {
        let (mut instance, root) = instance_with_root();
        let parent = AgentId(2);
        let child = AgentId(3);
        let worker = AgentKind::from_static("worker");
        instance
            .build_agent(
                parent,
                &worker,
                Message::text("spawn children"),
                AgentPlacement::Child(parent),
                Vec::new(),
            )
            .expect("parent builds");
        assert!(instance.state.get_mut().insert_child(
            root,
            parent,
            worker.clone(),
            Some("epsilon".to_owned()),
            timeout(),
        ));
        instance
            .build_agent(
                child,
                &worker,
                Message::text("nested work"),
                AgentPlacement::Child(child),
                Vec::new(),
            )
            .expect("child builds");
        assert!(instance.state.get_mut().insert_child(
            parent,
            child,
            worker,
            Some("nested".to_owned()),
            timeout(),
        ));
        instance.timeouts.arm::<ImmediateTimer>(parent, timeout());
        instance.timeouts.arm::<ImmediateTimer>(child, timeout());

        instance.route_outcome(
            child,
            TickOutcome::Yielded {
                text: "nested work complete".to_owned(),
            },
        );
        instance.route_outcome(
            parent,
            TickOutcome::Yielded {
                text: "waiting for nested work".to_owned(),
            },
        );

        assert!(instance.state.get().contains(parent));
        assert!(!instance.state.get().contains(child));
        assert!(instance.slots.has_inbox(parent));
        assert!(instance.slots.activate_inbox(parent));

        instance.route_outcome(
            parent,
            TickOutcome::Yielded {
                text: "aggregated nested result".to_owned(),
            },
        );
        assert!(!instance.state.get().contains(parent));
        assert!(!instance.timeouts.has_pending());
    }
}
