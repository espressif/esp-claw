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
use super::state::ToolBlockVerdict;
use super::task_state::TaskAction;
use super::{AgentId, BaseAgent, IterationIdAllocator};
use claw_permission::Grant;

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    /// Queue a command. The single inbound entry point.
    pub(crate) fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        self.state.get_mut().task_mut().enqueue_command(command)
    }

    pub(crate) fn push_task_input(&mut self, text: String) {
        self.state.get_mut().task_mut().enqueue_task_input(text);
    }

    pub(crate) fn abort_handle(&self) -> AgentAbortHandle {
        self.interruption.handle()
    }

    /// Deliver a finished subagent's result as ordinary task input.
    pub(crate) fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool) {
        let status = if ok { "ok" } else { "failed" };
        self.push_task_input(format!("[subagent {child} {status}] {text}"));
    }

    pub(super) fn drain_inbox(&mut self) {
        loop {
            let action = match self.state.get_mut().task_mut().pop_action() {
                Ok(action) => action,
                Err(error) => {
                    tracing::error!(
                        name: "task_mailbox_invariant_failed",
                        detail = %error,
                    );
                    self.fail_with(AgentRunError::TaskStateInvariant);
                    break;
                }
            };
            let Some(action) = action else {
                break;
            };
            self.apply_action(action);
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
                state.task_mut().enqueue_control(signal);
            }
        }
    }

    fn apply_action(&mut self, action: TaskAction) {
        match action {
            TaskAction::TaskInput { text, starts_task } => {
                self.append_task_input(&text, starts_task);
            }
            TaskAction::Cancel => {
                self.transcript.discard_open_turn();
                self.interruption.clear();
                self.outcome = Some(TickOutcome::Cancelled);
            }
            TaskAction::ApprovalResult {
                decision,
                grant_signatures,
            } => {
                let marker = match &decision {
                    ApprovalDecision::Approved => "[approval] approved by the human.".to_owned(),
                    ApprovalDecision::Rejected(reason) => {
                        format!("[approval] rejected by the human: {reason}")
                    }
                };
                self.transcript.append_user(&marker, false);
                self.record_grants(&decision, grant_signatures);
            }
            TaskAction::EndConversation { final_message } => {
                self.transcript.commit_ended(&final_message);
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
                    self.state.get_mut().task_mut().finish_task();
                    self.outcome = Some(TickOutcome::Yielded { text: answer.text });
                }
                CompletedKind::Tools(tools) => {
                    // A tool round is a non-terminal iteration: keep it in the
                    // open turn so the whole user request stays one user-started
                    // group, committed only by the terminal iteration.
                    self.transcript
                        .append_patch(&tools.appended.into_json_array());
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
        let signatures = pending.into_iter().map(|(_, sig)| sig).collect();
        if let Err(error) = self.state.get_mut().task_mut().await_approval(signatures) {
            tracing::error!(
                name: "task_phase_transition_failed",
                transition = "await_approval",
                detail = %error,
            );
            self.fail_with(AgentRunError::TaskStateInvariant);
            return;
        }
        self.outcome = Some(TickOutcome::AwaitingApproval { summary });
    }

    fn record_grants(&mut self, decision: &ApprovalDecision, signatures: Vec<String>) {
        let state = self.state.get_mut();
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
        self.state.get_mut().task_mut().finish_task();
        self.outcome = Some(TickOutcome::Failed(error));
    }

    fn merge_preempt_patch(&mut self, outcome: PreemptedOutcome) {
        if outcome.produced.is_empty() {
            return;
        }
        if has_dangling_tool_calls(outcome.produced.as_slice()) {
            let tool_call_count = outcome
                .produced
                .as_slice()
                .iter()
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
        self.transcript
            .append_patch(&outcome.produced.into_json_array());
    }

    fn append_task_input(&mut self, text: &str, starts_task: bool) {
        if starts_task {
            let state = self.state.get_mut();
            state.iterations = IterationIdAllocator::new();
            self.outcome = None;
        }
        self.transcript.append_user(text, starts_task);
    }
}

fn has_dangling_tool_calls(items: &[Value]) -> bool {
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
