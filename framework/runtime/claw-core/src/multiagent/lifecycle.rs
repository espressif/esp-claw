use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentCommand, TickOutcome};
use crate::protocol::{AgentId, Message};

use super::agent_control::AgentMessageDeliveryError;
use super::model::SubagentResult;
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
        let (kind, name, goal) = spec.into_parts();
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
                let inserted = self
                    .state
                    .get_mut()
                    .insert_child(parent, id, kind.clone(), name);
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
                tracing::info!(
                    name: "spawn_materialized",
                    parent_agent = %parent,
                    child_agent = %id,
                    kind = %kind.as_str(),
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

        self.deliver_subagent_result(parent_id, id, text, ok);
        self.delete_subtree(id);
        DriveOutput::default()
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
    use crate::protocol::{AgentId, AgentKind, SessionId};

    use super::super::{AgentIdAllocator, MultiagentRuntime, MultiagentState, ROOT_AGENT_KIND};

    type TestInstance = MultiagentRuntime<MemFs, RealHttp, ImmediateTimer>;

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
}
