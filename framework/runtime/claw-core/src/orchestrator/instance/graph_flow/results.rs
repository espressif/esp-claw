use std::collections::VecDeque;

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentId, TerminationPolicy, TickOutcome};

use super::super::output::DriveOutput;
use super::super::scheduler::SubagentResult;
use super::super::OrchestratorInstance;

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::instance) fn route_outcome(
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

    pub(super) fn deliver_or_mailbox_subagent_result(
        &mut self,
        parent: AgentId,
        child: AgentId,
        text: String,
        ok: bool,
    ) {
        if !self.state.get().graph.contains(parent) {
            return;
        }
        if self.state.get().scheduler.is_awaiting_approval(parent) {
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = true,
            );
            self.state
                .get_mut()
                .scheduler
                .queue_subagent_result(SubagentResult {
                    parent,
                    child,
                    text,
                    ok,
                });
            return;
        }
        if let Some(parent_agent) = self.registry.get_mut(parent) {
            parent_agent.deliver_child_result(child, text, ok);
            self.enqueue(parent);
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = false,
            );
        } else {
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = true,
            );
            self.state
                .get_mut()
                .scheduler
                .queue_subagent_result(SubagentResult {
                    parent,
                    child,
                    text,
                    ok,
                });
        }
    }

    pub(in crate::orchestrator::instance) fn flush_subagent_result_mailbox(&mut self) {
        if !self.state.get().scheduler.has_subagent_results() {
            return;
        }
        let mut pending = VecDeque::new();
        let results = self.state.get_mut().scheduler.take_subagent_results();
        for result in results {
            if !self.state.get().graph.contains(result.parent) {
                continue;
            }
            if self
                .state
                .get()
                .scheduler
                .is_awaiting_approval(result.parent)
            {
                pending.push_back(result);
                continue;
            }
            let parent = result.parent;
            if let Some(parent_agent) = self.registry.get_mut(parent) {
                parent_agent.deliver_child_result(result.child, result.text, result.ok);
                self.enqueue(parent);
            } else {
                pending.push_back(result);
            }
        }
        self.state
            .get_mut()
            .scheduler
            .replace_subagent_results(pending);
    }

    fn route_yielded(&mut self, id: AgentId, text: String) -> DriveOutput {
        let Some((parent, termination)) = self
            .state
            .get()
            .graph
            .node(id)
            .map(|meta| (meta.parent(), meta.termination()))
        else {
            return DriveOutput::default();
        };
        let Some(parent_id) = parent else {
            return DriveOutput::default();
        };

        self.deliver_or_mailbox_subagent_result(parent_id, id, text, true);

        if termination == TerminationPolicy::Manual {
            tracing::info!(name: "manual_yielded", "");
        } else {
            self.delete_subtree(id);
        }
        DriveOutput::default()
    }

    fn route_terminal(&mut self, id: AgentId, text: String, ok: bool) -> DriveOutput {
        let Some(parent) = self.state.get().graph.node(id).map(|meta| meta.parent()) else {
            return DriveOutput::default();
        };
        let Some(parent_id) = parent else {
            return DriveOutput::message(text);
        };

        self.deliver_or_mailbox_subagent_result(parent_id, id, text, ok);
        self.delete_subtree(id);
        DriveOutput::default()
    }

    fn route_cancelled(&mut self, id: AgentId) -> DriveOutput {
        if self
            .state
            .get()
            .graph
            .node(id)
            .and_then(|meta| meta.parent())
            .is_none()
        {
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
    use claw_tool::ToolRegistry;

    use crate::agent::{AgentId, AgentIdAllocator, AgentKind, FsAgentFactory, TickOutcome};
    use crate::config::ClawApiManager;
    use crate::session::SessionId;

    use super::super::super::state::OrchestratorInstanceState;
    use super::super::super::{OrchestratorInstance, ROOT_AGENT_KIND};

    type TestInstance = OrchestratorInstance<MemFs, RealHttp, ImmediateTimer>;

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
        let mut instance = OrchestratorInstance::new(
            SessionId::new(1),
            Rc::new(factory),
            AgentIdAllocator::new(),
            OrchestratorInstanceState::default(),
        );
        let root = AgentId(1);
        assert!(instance
            .state
            .get_mut()
            .graph
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
