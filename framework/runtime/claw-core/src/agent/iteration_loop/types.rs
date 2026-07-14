use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use serde_json::Value;
use strum::IntoStaticStr;

use claw_api::ChatError;
use claw_tool::{ToolGate, ToolSetError, ToolSetHandle};

use crate::protocol::IterationId;

/// Errors from one [`super::IterationLoop::run`] step.
#[derive(Clone, Debug, IntoStaticStr, thiserror::Error)]
pub(crate) enum IterationLoopError {
    #[strum(serialize = "missing_assistant_message")]
    #[error("LLM tool-call response missing raw assistant message JSON")]
    MissingAssistantMessage,
    #[strum(serialize = "malformed_assistant_message")]
    #[error("LLM raw assistant message JSON is not valid JSON")]
    MalformedAssistantMessage,
    #[strum(serialize = "chat")]
    #[error(transparent)]
    Chat(#[from] ChatError),
    #[strum(serialize = "tools")]
    #[error(transparent)]
    Tools(#[from] ToolSetError),
}

/// Checkpoint where preemption was detected. The iteration is terminal at this point.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum IterationCheckpoint {
    BeforeLlmHttp,
    InLlmHttpAbort,
    AfterLlmBeforeTool,
    BeforeTool,
}

/// Owned message batch appended by one completed step (assistant/tool round).
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub(crate) struct AppendedMessages {
    messages: Vec<Value>,
}

impl AppendedMessages {
    pub(crate) fn empty() -> Self {
        Self {
            messages: Vec::new(),
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.messages.is_empty()
    }

    pub(super) fn push(&mut self, message: Value) {
        self.messages.push(message);
    }

    pub(crate) fn as_slice(&self) -> &[Value] {
        &self.messages
    }

    pub(crate) fn into_json_array(self) -> Value {
        Value::Array(self.messages)
    }

    pub(crate) fn replace_tool_result(
        &mut self,
        tool_call_id: &str,
        content: String,
        ok: bool,
    ) -> bool {
        let Some(message) = self.messages.iter_mut().find(|message| {
            message.get("role").and_then(Value::as_str) == Some("tool")
                && message.get("tool_call_id").and_then(Value::as_str) == Some(tool_call_id)
        }) else {
            return false;
        };
        *message = serde_json::json!({
            "role": "tool",
            "tool_call_id": tool_call_id,
            "content": content,
            "is_error": !ok,
        });
        true
    }
}

/// Inputs for exactly one [`super::IterationLoop::run`]: chat fields + tools.
pub(crate) struct IterationStep<'a> {
    pub iteration_id: IterationId,
    pub system_prompt: &'a str,
    pub messages: &'a Value,
    /// Ephemeral trailing messages for this request only (never persisted),
    /// appended after `messages`. Empty when there is nothing to nudge.
    pub reminders: &'a [Value],
    /// The tool view for this step. It stays stable for the whole iteration.
    pub tools: &'a ToolSetHandle<'a>,
    /// Permission gate consulted before each call after soft-hide passes. On
    /// `Deny` the call is refused; on `Ask` it is held for human approval
    /// (surfaced via [`ToolRun::approval`]) and not run.
    pub gate: &'a dyn ToolGate,
}

/// Terminal outcome of exactly one [`super::IterationLoop::run`] (completed or preempted).
#[derive(Clone, Debug)]
pub(crate) enum IterationOutcome {
    Completed(CompletedOutcome),
    Preempted(PreemptedOutcome),
}

/// One [`super::IterationLoop::run`] step: [`IterationOutcome`] or [`IterationLoopError`].
pub(crate) type IterationResult = Result<IterationOutcome, IterationLoopError>;

/// Successful iteration: plain-text answer or executed tool round.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct CompletedOutcome {
    pub iteration_id: IterationId,
    pub kind: CompletedKind,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum CompletedKind {
    PlainText(PlainTextOutcome),
    Tools(ToolsOutcome),
}

/// One executed tool call (for iteration-level observers above this layer).
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ToolRun {
    pub name: String,
    pub ok: bool,
    pub(super) disposition: ToolRunDisposition,
}

impl ToolRun {
    /// True when the call was refused by soft-hide gating (not in the tool set's
    /// active allow-set) instead of being invoked.
    pub(crate) fn is_blocked(&self) -> bool {
        matches!(self.disposition, ToolRunDisposition::Blocked)
    }

    pub(crate) fn approval(&self) -> Option<&PendingApproval> {
        match &self.disposition {
            ToolRunDisposition::AwaitingApproval(approval) => Some(approval),
            ToolRunDisposition::Executed | ToolRunDisposition::Blocked => None,
        }
    }

    pub(crate) fn into_approval(self) -> Option<(String, PendingApproval)> {
        match self.disposition {
            ToolRunDisposition::AwaitingApproval(approval) => Some((self.name, approval)),
            ToolRunDisposition::Executed | ToolRunDisposition::Blocked => None,
        }
    }
}

/// Why a tool run did or did not execute. This keeps mutually exclusive
/// execution states in one field instead of pairing booleans with optional
/// payloads.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) enum ToolRunDisposition {
    Executed,
    Blocked,
    AwaitingApproval(PendingApproval),
}

/// Everything required to resume exactly one held tool call after the caller
/// resolves its permission request.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PendingApproval {
    pub(crate) tool_call_id: String,
    pub(crate) arguments_json: String,
    pub(crate) summary: String,
    pub(crate) signature: String,
}

/// The model issued tool calls and they were executed.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ToolsOutcome {
    /// Assistant + tool messages produced this step (iteration layer merges).
    pub appended: AppendedMessages,
    pub runs: Vec<ToolRun>,
}

impl ToolsOutcome {
    pub(crate) fn next_approval(&self) -> Option<(&str, &PendingApproval)> {
        self.runs
            .iter()
            .find_map(|run| run.approval().map(|approval| (run.name.as_str(), approval)))
    }

    #[cfg(test)]
    pub(crate) fn pending_for_test(signature: &str) -> Self {
        let tool_call_id = "pending-call";
        let mut appended = AppendedMessages::empty();
        appended.push(serde_json::json!({
            "role": "assistant",
            "tool_calls": [{
                "id": tool_call_id,
                "type": "function",
                "function": {
                    "name": "pending_tool",
                    "arguments": "{}",
                },
            }],
        }));
        appended.push(serde_json::json!({
            "role": "tool",
            "tool_call_id": tool_call_id,
            "content": "approval required",
            "is_error": true,
        }));
        Self {
            appended,
            runs: vec![ToolRun {
                name: "pending_tool".to_owned(),
                ok: false,
                disposition: ToolRunDisposition::AwaitingApproval(PendingApproval {
                    tool_call_id: tool_call_id.to_owned(),
                    arguments_json: "{}".to_owned(),
                    summary: "approval required".to_owned(),
                    signature: signature.to_owned(),
                }),
            }],
        }
    }
}

/// The model returned a final plain-text answer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PlainTextOutcome {
    pub text: String,
    pub raw_message_json: Option<String>,
}

/// Iteration ended at a preempt checkpoint.
///
/// `produced` carries assistant/tool messages already materialized this step
/// (not interrupt message content). Upper layers decide whether to merge them.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PreemptedOutcome {
    pub iteration_id: IterationId,
    pub checkpoint: IterationCheckpoint,
    pub produced: AppendedMessages,
}

/// User interrupt surface for one in-flight iteration. No message payloads here.
///
/// Contract with [`claw_interface::http::ClawHttp`]:
/// - Upper layer sets `interrupt_flag` to request cooperative abort.
/// - HTTP polls the flag and returns [`claw_interface::http::HttpError::Aborted`]
///   without clearing it (`claw_sys` / ESP HTTP keeps the flag intact).
/// - [`super::IterationLoop`] consumes the flag via `swap(false)` when ending preempted.
pub(crate) trait InterruptionControl {
    /// Polled at checkpoints (consume) and passed to in-flight LLM HTTP (cooperative abort).
    fn interrupt_flag(&self) -> &Arc<AtomicBool>;
}

pub(super) fn take_interrupt(interruption: &dyn InterruptionControl) -> bool {
    interruption.interrupt_flag().swap(false, Ordering::AcqRel)
}

pub(super) fn check_preempt_at_checkpoint(
    interruption: &dyn InterruptionControl,
    iteration_id: IterationId,
    checkpoint: IterationCheckpoint,
    produced: AppendedMessages,
) -> Option<PreemptedOutcome> {
    if !take_interrupt(interruption) {
        return None;
    }

    Some(PreemptedOutcome {
        iteration_id,
        checkpoint,
        produced,
    })
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::AppendedMessages;

    #[test]
    fn appended_messages_can_only_materialize_as_an_array() {
        let mut messages = AppendedMessages::empty();
        messages.push(json!({ "role": "assistant", "content": "working" }));
        messages.push(json!({ "role": "tool", "content": "done" }));

        assert_eq!(
            messages.into_json_array(),
            json!([
                { "role": "assistant", "content": "working" },
                { "role": "tool", "content": "done" }
            ])
        );
    }
}
