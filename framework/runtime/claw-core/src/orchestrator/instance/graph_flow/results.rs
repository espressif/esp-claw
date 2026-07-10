use std::collections::VecDeque;

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentId, CancelReason, TerminationPolicy, TickOutcome};

use super::super::model::{DriveOutput, RootReply, SubagentResult};
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
            TickOutcome::AwaitingApproval {
                id: approval,
                summary,
            } => self.park_approval(id, approval, summary),
            TickOutcome::Yielded { text } => {
                DriveOutput::replies(self.route_result(id, text, true, false))
            }
            TickOutcome::Ended { final_message } => {
                DriveOutput::replies(self.route_result(id, final_message, true, true))
            }
            TickOutcome::Cancelled { reason } => self.route_cancelled(id, reason),
            TickOutcome::Failed(error) => DriveOutput::replies(self.route_result(
                id,
                format!("[failed: {error:?}]"),
                false,
                true,
            )),
        }
    }

    pub(super) fn deliver_or_mailbox_subagent_result(
        &mut self,
        parent: AgentId,
        child: AgentId,
        text: String,
        ok: bool,
    ) {
        if !self.state.get().meta.contains_key(&parent) {
            return;
        }
        if self.state.get().parked_approvals.contains_key(&parent) {
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = true,
            );
            self.state
                .get_mut()
                .subagent_result_mailbox
                .push_back(SubagentResult {
                    parent,
                    child,
                    text,
                    ok,
                });
            return;
        }
        if let Some(parent_agent) = self.state.get_mut().registry.get_mut(parent) {
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
                .subagent_result_mailbox
                .push_back(SubagentResult {
                    parent,
                    child,
                    text,
                    ok,
                });
        }
    }

    pub(in crate::orchestrator::instance) fn flush_subagent_result_mailbox(&mut self) {
        if self.state.get().subagent_result_mailbox.is_empty() {
            return;
        }
        let mut pending = VecDeque::new();
        while let Some(result) = self.pop_subagent_result_mailbox() {
            if !self.state.get().meta.contains_key(&result.parent) {
                continue;
            }
            if self
                .state
                .get()
                .parked_approvals
                .contains_key(&result.parent)
            {
                pending.push_back(result);
                continue;
            }
            let parent = result.parent;
            if let Some(parent_agent) = self.state.get_mut().registry.get_mut(parent) {
                parent_agent.deliver_child_result(result.child, result.text, result.ok);
                self.enqueue(parent);
            } else {
                pending.push_back(result);
            }
        }
        self.state.get_mut().subagent_result_mailbox = pending;
    }

    fn route_result(&mut self, id: AgentId, text: String, ok: bool, ended: bool) -> Vec<RootReply> {
        let Some((parent, termination)) = self
            .state
            .get()
            .meta
            .get(&id)
            .map(|meta| (meta.parent, meta.termination))
        else {
            return Vec::new();
        };
        let Some(parent_id) = parent else {
            // A root plain answer streamed its text as Output fragments during the
            // iteration; a conversation-end closing message (`ended`) did not.
            return vec![RootReply {
                session: self.session,
                text,
                streamed: !ended,
            }];
        };

        self.deliver_or_mailbox_subagent_result(parent_id, id, text, ok);

        let keep_alive = termination == TerminationPolicy::Manual && ok && !ended;
        if keep_alive {
            tracing::info!(name: "manual_yielded", "");
        }
        if !keep_alive {
            self.delete_subtree(id);
        }
        Vec::new()
    }

    fn route_cancelled(&mut self, id: AgentId, _reason: CancelReason) -> DriveOutput {
        if self
            .state
            .get()
            .meta
            .get(&id)
            .and_then(|meta| meta.parent)
            .is_none()
        {
            return DriveOutput::default();
        }
        self.delete_subtree(id);
        DriveOutput::default()
    }

    fn pop_subagent_result_mailbox(&mut self) -> Option<SubagentResult> {
        if self.state.get().subagent_result_mailbox.is_empty() {
            return None;
        }
        self.state.get_mut().subagent_result_mailbox.pop_front()
    }
}
