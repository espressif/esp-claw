use tracing::Instrument as _;

use claw_api::ChatRequest;
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_tool::ToolRunner;

use crate::event::{EventSink, SessionEvent};

use super::tool_round::{append_assistant_tool_calls, run_tool_calls, ToolRoundResult};
use super::types::{
    check_preempt_at_checkpoint, take_interrupt, AppendedMessages, CompletedKind, CompletedOutcome,
    IterationCheckpoint, IterationLoopError, IterationOutcome, IterationResult, IterationStep,
    PlainTextOutcome, PreemptedOutcome, ToolsOutcome,
};
use super::IterationLoop;

/// Emits [`SessionEvent::IterationEnded`] when dropped, so every `run_one_iteration`
/// exit path closes the bracket its [`SessionEvent::IterationStarted`] opened.
struct IterationBracket<'a> {
    events: &'a EventSink,
}

impl Drop for IterationBracket<'_> {
    fn drop(&mut self) {
        self.events.emit(SessionEvent::IterationEnded);
    }
}

impl<H: ClawHttp, Timer: ClawTimer> IterationLoop<'_, H, Timer> {
    /// Execute exactly one iteration: LLM chat -> optional tool execution.
    pub(crate) async fn run(self, step: IterationStep<'_>) -> IterationResult {
        let span = tracing::info_span!("iteration_loop", run.iteration = %step.iteration_id);
        run_one_iteration(self, step).instrument(span).await
    }
}

async fn run_one_iteration<H: ClawHttp, Timer: ClawTimer>(
    loop_: IterationLoop<'_, H, Timer>,
    step: IterationStep<'_>,
) -> IterationResult {
    let iteration_id = step.iteration_id;
    // Open the iteration event bracket; the guard closes it (IterationEnded) on
    // every return path below.
    let events = loop_.events;
    events.emit(SessionEvent::IterationStarted {
        iteration: iteration_id,
    });
    let _bracket = IterationBracket { events };
    let mut appended = AppendedMessages::empty();

    if let Some(outcome) = check_preempt_at_checkpoint(
        loop_.interruption,
        iteration_id,
        IterationCheckpoint::BeforeLlmHttp,
        AppendedMessages::empty(),
    ) {
        tracing::warn!(name: "preempted", checkpoint = "before_llm_http");
        return Ok(IterationOutcome::Preempted(outcome));
    }

    let chat_request = ChatRequest {
        system_prompt: step.system_prompt.as_ref(),
        messages: step.messages.0,
        reminders: step.reminders,
        tools_json: Some(step.tools.schemas_json()),
        retry: loop_.retry,
    };
    let cancel = Cancel::new(loop_.interruption.interrupt_flag().as_ref());
    let llm_response = match loop_.llm.chat(&chat_request, cancel).await {
        Ok(resp) => resp,
        Err(llm_err) => {
            if take_interrupt(loop_.interruption) || llm_err.is_aborted() {
                tracing::warn!(name: "preempted", checkpoint = "in_llm_http_abort");
                return Ok(IterationOutcome::Preempted(PreemptedOutcome {
                    iteration_id,
                    checkpoint: IterationCheckpoint::InLlmHttpAbort,
                    produced: AppendedMessages::empty(),
                }));
            }
            tracing::error!(name: "chat_failed", kind = "chat");
            return Err(IterationLoopError::Chat(llm_err));
        }
    };

    // Reasoning is emitted first within the iteration (before any tools), matching
    // the provider ordering `reasoning -> msg? -> tools?`. A no-op when empty or
    // the sink is disabled.
    if let Some(reasoning) = llm_response.reasoning_content.as_deref() {
        events.emit_reasoning(reasoning);
    }

    if llm_response.tool_calls.is_empty() {
        if let Some(outcome) = check_preempt_at_checkpoint(
            loop_.interruption,
            iteration_id,
            IterationCheckpoint::AfterLlmBeforeTool,
            AppendedMessages::empty(),
        ) {
            tracing::warn!(name: "preempted", checkpoint = "after_llm_before_tool");
            return Ok(IterationOutcome::Preempted(outcome));
        }

        let text = llm_response
            .text
            .clone()
            .ok_or(IterationLoopError::MalformedAssistantMessage)?;
        tracing::info!(name: "completed", output_bytes = text.len() as u64);
        return Ok(IterationOutcome::Completed(CompletedOutcome {
            iteration_id,
            kind: CompletedKind::PlainText(PlainTextOutcome {
                text,
                raw_message_json: llm_response.raw_message_json.clone(),
            }),
        }));
    }

    if let Some(outcome) = check_preempt_at_checkpoint(
        loop_.interruption,
        iteration_id,
        IterationCheckpoint::AfterLlmBeforeTool,
        AppendedMessages::empty(),
    ) {
        tracing::warn!(name: "preempted", checkpoint = "after_llm_before_tool");
        return Ok(IterationOutcome::Preempted(outcome));
    }

    tracing::info!(
        name: "tool_calls",
        count = llm_response.tool_calls.len() as u64,
    );

    // Tool names for this iteration. Only build the payload when the sink will
    // keep it (disabled subagent sinks would drop the clones).
    if events.is_enabled() {
        events.emit(SessionEvent::Tools {
            names: llm_response
                .tool_calls
                .iter()
                .map(|tc| tc.display_name().to_string())
                .collect(),
        });
    }

    if let Err(err) = append_assistant_tool_calls(&mut appended.0, &llm_response) {
        let kind: &'static str = (&err).into();
        tracing::error!(name: "assistant_tool_calls_invalid", kind);
        return Err(err);
    }

    let runner = ToolRunner::new(step.tools, Some(step.gate));
    match run_tool_calls(
        loop_.interruption,
        &runner,
        &mut appended,
        &llm_response,
        iteration_id,
    )
    .await
    {
        ToolRoundResult::Completed { runs } => {
            tracing::info!(name: "tool_round_completed", count = runs.len() as u64);
            Ok(IterationOutcome::Completed(CompletedOutcome {
                iteration_id,
                kind: CompletedKind::Tools(ToolsOutcome { appended, runs }),
            }))
        }
        ToolRoundResult::Preempted(outcome) => {
            tracing::warn!(name: "preempted", checkpoint = "before_tool");
            Ok(IterationOutcome::Preempted(outcome))
        }
        ToolRoundResult::Failed(err) => {
            let kind: &'static str = (&err).into();
            tracing::error!(name: "tool_round_failed", kind);
            Err(err)
        }
    }
}
