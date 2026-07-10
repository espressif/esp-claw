use std::collections::HashSet;

use claw_interface::{ClawHttp, ClawTimer};
use serde_json::Value;

use crate::agent::iteration_loop::{
    CompletedKind, CompletedOutcome, IterationOutcome, IterationResult, PreemptedOutcome, ToolRun,
};
use crate::agent::tools::ControlSignal;
use crate::memory::AssistantCommit;

use super::command::{
    AgentCommand, AgentCommandError, AgentRunError, ApprovalDecision, TickOutcome,
};
use super::control::AgentAbortHandle;
use super::state::{classify, AgentLifecycle, Inbound, ToolBlockVerdict};
use super::{BaseAgent, IterationIdAllocator};
use claw_permission::Grant;

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    /// Queue a command. The single inbound entry point.
    pub fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        let next = classify(self.state.get().projected_lifecycle, &command)?;
        let state = self.state.get_mut();
        state.projected_lifecycle = next;
        state.inbox.push_back(Inbound::Command(command));
        Ok(())
    }

    pub(crate) fn push_task_input(&mut self, text: impl Into<String>) {
        let state = self.state.get_mut();
        if state.projected_lifecycle == AgentLifecycle::Idle {
            state.projected_lifecycle = AgentLifecycle::Running;
        }
        state.inbox.push_back(Inbound::TaskInput(text.into()));
    }

    pub fn abort_handle(&self) -> AgentAbortHandle {
        self.interruption.handle()
    }

    pub(super) fn drain_inbox(&mut self) {
        loop {
            let inbound = if self.state.get().inbox.is_empty() {
                None
            } else {
                self.state.get_mut().inbox.pop_front()
            };
            let Some(inbound) = inbound else {
                break;
            };
            self.apply_inbound(inbound);
        }
    }

    pub(super) fn drain_control_signals(&mut self) {
        let signals: Vec<ControlSignal> = {
            let mut sink = self
                .control
                .lock()
                .unwrap_or_else(|poison| poison.into_inner());
            sink.drain(..).collect()
        };
        if !signals.is_empty() {
            let state = self.state.get_mut();
            for signal in signals {
                state.inbox.push_back(Inbound::Control(signal));
            }
        }
    }

    fn apply_inbound(&mut self, inbound: Inbound) {
        match inbound {
            Inbound::Command(AgentCommand::AppendMessage(text)) | Inbound::TaskInput(text) => {
                self.append_task_input(&text);
            }
            Inbound::Command(AgentCommand::Cancel { reason }) => {
                self.transcript.discard_open_turn();
                self.state.get_mut().pending_grant_signatures.clear();
                self.interruption.clear();
                self.state.get_mut().lifecycle = AgentLifecycle::Idle;
                self.outcome = Some(TickOutcome::Cancelled { reason });
            }
            Inbound::Command(AgentCommand::ApprovalResult { id, decision }) => {
                let marker = match &decision {
                    ApprovalDecision::Approved => {
                        format!("[approval {id}] approved by the human.")
                    }
                    ApprovalDecision::Rejected(reason) => {
                        format!("[approval {id}] rejected by the human: {reason}")
                    }
                };
                self.transcript.append_user(&marker, false);
                self.record_grants(&decision);
                self.state.get_mut().lifecycle = AgentLifecycle::Running;
            }
            Inbound::Control(ControlSignal::EndConversation { final_message }) => {
                self.transcript.commit_ended(&final_message);
                self.state.get_mut().lifecycle = AgentLifecycle::Idle;
                self.outcome = Some(TickOutcome::Ended { final_message });
            }
        }
    }

    pub(super) fn reduce_outcome(&mut self, outcome: IterationResult) {
        match outcome {
            Ok(IterationOutcome::Completed(CompletedOutcome { kind, .. })) => match kind {
                CompletedKind::PlainText(answer) => {
                    let commit = match answer.raw_message_json.as_deref() {
                        Some(raw) => AssistantCommit::RawJson(raw),
                        None => AssistantCommit::PlainText(&answer.text),
                    };
                    self.transcript.commit_assistant(commit);
                    self.state.get_mut().lifecycle = AgentLifecycle::Idle;
                    self.outcome = Some(TickOutcome::Yielded { text: answer.text });
                }
                CompletedKind::Tools(tools) => {
                    // A tool round is a non-terminal iteration: keep it in the
                    // open turn so the whole user request stays one user-started
                    // group, committed only by the terminal iteration.
                    self.transcript.append_patch(&tools.appended.0);
                    self.apply_tool_block_policy(&tools.runs);
                    self.maybe_raise_approval(&tools.runs);
                }
            },
            Ok(IterationOutcome::Preempted(outcome)) => {
                self.merge_preempt_patch(outcome);
            }
            Err(error) => self.fail_with(error.into()),
        }
    }

    fn apply_tool_block_policy(&mut self, runs: &[ToolRun]) {
        let blocked: Vec<&str> = runs
            .iter()
            .filter(|run| run.is_blocked())
            .map(|run| run.name.as_str())
            .collect();
        if !blocked.is_empty() {
            tracing::warn!(name: "tool_gate_blocked", count = blocked.len() as u64);
        }
        if let ToolBlockVerdict::Exhausted { name } =
            self.state.get_mut().block_policy.record_round(&blocked)
        {
            self.fail_with(AgentRunError::ToolNotPermitted { name });
        }
    }

    fn maybe_raise_approval(&mut self, runs: &[ToolRun]) {
        if self.outcome.is_some() {
            return;
        }
        let pending: Vec<(String, String)> = runs
            .iter()
            .filter_map(|run| {
                run.approval()
                    .map(|approval| (approval.summary.clone(), approval.signature.clone()))
            })
            .collect();
        let Some((summary, _)) = pending.first().cloned() else {
            return;
        };
        let id = {
            let state = self.state.get_mut();
            state.pending_grant_signatures = pending.into_iter().map(|(_, sig)| sig).collect();
            let id = state.approvals.next();
            state.lifecycle = AgentLifecycle::AwaitingApproval(id);
            id
        };
        self.outcome = Some(TickOutcome::AwaitingApproval { id, summary });
    }

    fn record_grants(&mut self, decision: &ApprovalDecision) {
        let state = self.state.get_mut();
        let signatures = std::mem::take(&mut state.pending_grant_signatures);
        let grant = match decision {
            ApprovalDecision::Approved => Grant::Granted,
            ApprovalDecision::Rejected(reason) => Grant::Denied(reason.clone()),
        };
        let grants = &mut state.permission_grants;
        for signature in signatures {
            match &grant {
                Grant::Granted => grants.grant(signature),
                Grant::Denied(reason) => grants.deny(signature, reason.clone()),
            }
        }
    }

    fn fail_with(&mut self, error: AgentRunError) {
        self.state.get_mut().lifecycle = AgentLifecycle::Idle;
        self.outcome = Some(TickOutcome::Failed(error));
    }

    fn merge_preempt_patch(&mut self, outcome: PreemptedOutcome) {
        if outcome.produced.is_empty() {
            return;
        }
        if has_dangling_tool_calls(&outcome.produced.0) {
            let tool_call_count = outcome
                .produced
                .0
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|message| message.get("tool_calls").and_then(Value::as_array))
                .map(|calls| calls.len())
                .sum::<usize>();
            tracing::warn!(
                name: "preempt_patch_dropped",
                tool_call_count = tool_call_count as u64,
            );
            return;
        }
        // Preemption ends the iteration but the turn continues in a fresh
        // iteration, so keep the salvaged work in the open turn rather than
        // closing it into its own user-less group.
        self.transcript.append_patch(&outcome.produced.0);
    }

    fn append_task_input(&mut self, text: &str) {
        let starts_task = self.state.get().lifecycle == AgentLifecycle::Idle;
        if starts_task {
            let state = self.state.get_mut();
            state.iterations = IterationIdAllocator::new();
            state.lifecycle = AgentLifecycle::Running;
            self.outcome = None;
        }
        self.transcript.append_user(text, starts_task);
    }
}

fn has_dangling_tool_calls(patch: &Value) -> bool {
    let Some(items) = patch.as_array() else {
        return false;
    };
    let mut expected: Vec<&str> = Vec::new();
    let mut satisfied: HashSet<&str> = HashSet::new();
    for message in items {
        if let Some(calls) = message.get("tool_calls").and_then(Value::as_array) {
            for call in calls {
                if let Some(id) = call.get("id").and_then(Value::as_str) {
                    expected.push(id);
                }
            }
        }
        if let Some(id) = message.get("tool_call_id").and_then(Value::as_str) {
            satisfied.insert(id);
        }
    }
    expected.iter().any(|id| !satisfied.contains(id))
}
