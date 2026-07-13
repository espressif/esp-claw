use tracing::Instrument as _;

use claw_api::LlmResponse;
use claw_tool::{RawToolInvocation, ToolInvocation, ToolRunOutcome, ToolRunner};

use super::types::{
    check_preempt_at_checkpoint, AppendedMessages, InterruptionControl, IterationCheckpoint,
    IterationLoopError, PreemptedOutcome, ToolRun, ToolRunDisposition,
};
use super::IterationId;

pub(super) enum ToolRoundResult {
    Completed { runs: Vec<ToolRun> },
    Preempted(PreemptedOutcome),
}

pub(super) fn append_assistant_tool_calls(
    messages: &mut AppendedMessages,
    response: &LlmResponse,
) -> Result<(), IterationLoopError> {
    let Some(raw) = response
        .raw_message_json
        .as_deref()
        .filter(|s| !s.is_empty())
    else {
        return Err(IterationLoopError::MissingAssistantMessage);
    };
    let Ok(assistant) = serde_json::from_str(raw) else {
        return Err(IterationLoopError::MalformedAssistantMessage);
    };
    messages.push(assistant);
    Ok(())
}

pub(super) async fn run_tool_calls(
    interruption: &dyn InterruptionControl,
    runner: &ToolRunner<'_>,
    appended: &mut AppendedMessages,
    response: &LlmResponse,
    iteration_id: IterationId,
) -> ToolRoundResult {
    let mut runs: Vec<ToolRun> = Vec::with_capacity(response.tool_calls.len());

    for tc in &response.tool_calls {
        let span = tracing::info_span!("toolcall", tool = %tc.display_name());
        if let Some(outcome) = check_preempt_at_checkpoint(
            interruption,
            iteration_id,
            IterationCheckpoint::BeforeTool,
            appended.clone(),
        ) {
            span.in_scope(|| {
                tracing::warn!(name: "preempted", checkpoint = "before_tool");
            });
            return ToolRoundResult::Preempted(outcome);
        }
        span.in_scope(|| {
            tracing::info!(
                name: "arguments",
                argument_bytes = tc.arguments_json.len() as u64,
            );
        });

        // The runner owns the decision (soft-hide -> permission -> execute); the
        // loop owns preemption and message assembly. A matched tool message is
        // emitted for every call (even refused ones), so the patch stays well-formed
        // (no dangling tool_call ids).
        let call = match ToolInvocation::try_from(RawToolInvocation {
            id: Some(&tc.id),
            name: &tc.name,
            arguments_json: &tc.arguments_json,
        }) {
            Ok(call) => call,
            Err(error) => {
                span.in_scope(|| {
                    tracing::warn!(name: "parse_failed", kind = "invalid_invocation");
                });
                let content = error.to_string();
                push_tool_message(appended, &tc.id, content, false);
                span.in_scope(|| {
                    tracing::info!(name: "result", ok = false, blocked = false);
                });
                runs.push(ToolRun {
                    name: tc.display_name().to_string(),
                    ok: false,
                    disposition: ToolRunDisposition::Executed,
                });
                continue;
            }
        };
        let outcome = runner.run(&call).instrument(span.clone()).await;
        let (content, ok, blocked, approval) = match outcome {
            ToolRunOutcome::Ran { content, ok } => (content, ok, false, None),
            ToolRunOutcome::Blocked { content } => (content, false, true, None),
            ToolRunOutcome::ApprovalNeeded { content, approval } => {
                (content, false, false, Some(approval))
            }
        };
        span.in_scope(|| {
            if blocked || (!ok && approval.is_none()) {
                tracing::warn!(name: "result", ok, blocked);
            } else {
                tracing::info!(name: "result", ok, blocked);
            }
        });

        push_tool_message(appended, &tc.id, content, ok);
        let disposition = match approval {
            Some(approval) => ToolRunDisposition::AwaitingApproval(approval),
            None if blocked => ToolRunDisposition::Blocked,
            None => ToolRunDisposition::Executed,
        };
        runs.push(ToolRun {
            name: tc.display_name().to_string(),
            ok,
            disposition,
        });
    }

    ToolRoundResult::Completed { runs }
}

fn push_tool_message(
    appended: &mut AppendedMessages,
    id: &str,
    content: String,
    ok: bool,
) {
    let tool_message = serde_json::json!({
        "role": "tool",
        "tool_call_id": id,
        "content": content,
        "is_error": !ok,
    });

    appended.push(tool_message);
}
