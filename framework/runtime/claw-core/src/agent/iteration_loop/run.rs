use tracing::Instrument as _;

use claw_api::{ChatRequest, LlmDelta};
use claw_interface::http::StreamingHttp;
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_tool::ToolRunner;
use futures_lite::StreamExt;

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

impl<H: ClawHttp + StreamingHttp, Timer: ClawTimer> IterationLoop<'_, H, Timer> {
    /// Execute exactly one iteration: LLM chat -> optional tool execution.
    pub(crate) async fn run(self, step: IterationStep<'_>) -> IterationResult {
        let span = tracing::info_span!("iteration_loop", run.iteration = %step.iteration_id);
        run_one_iteration(self, step).instrument(span).await
    }
}

async fn run_one_iteration<H: ClawHttp + StreamingHttp, Timer: ClawTimer>(
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
        system_prompt: step.system_prompt,
        messages: step.messages,
        reminders: step.reminders,
        tools_json: Some(step.tools.schemas_json()),
        retry: loop_.retry,
    };
    let cancel = Cancel::new(loop_.interruption.interrupt_flag().as_ref());
    let max_attempts = u64::from(chat_request.retry.max_retries).saturating_add(1);
    let chat_span = tracing::info_span!("api.chat", purpose = "iteration", max_attempts);

    // Interpret a streaming/LLM error: a cooperative interrupt or provider abort
    // preempts this iteration; anything else is a chat failure.
    let interpret_chat_error = |llm_err: claw_api::ChatError| -> IterationResult {
        if take_interrupt(loop_.interruption) || llm_err.is_aborted() {
            tracing::warn!(name: "preempted", checkpoint = "in_llm_http_abort");
            return Ok(IterationOutcome::Preempted(PreemptedOutcome {
                iteration_id,
                checkpoint: IterationCheckpoint::InLlmHttpAbort,
                produced: AppendedMessages::empty(),
            }));
        }
        tracing::error!(name: "chat_failed", kind = "chat");
        Err(IterationLoopError::Chat(llm_err))
    };

    let llm_response = {
        let stream_result = loop_
            .llm
            .chat_stream(&chat_request, cancel)
            .instrument(chat_span.clone())
            .await;
        let mut stream = match stream_result {
            Ok(stream) => stream,
            Err(llm_err) => return interpret_chat_error(llm_err),
        };

        // The iteration loop owns streamed LLM fragments. The orchestrator emits
        // only messages it synthesizes outside this stream.
        let mut reasoning_emitted = 0usize;
        loop {
            let next = {
                StreamExt::next(&mut stream)
                    .instrument(chat_span.clone())
                    .await
            };
            match next {
                Some(Ok(LlmDelta::Reasoning(text))) => {
                    events.emit_reasoning_fragment(&text, &mut reasoning_emitted);
                }
                Some(Ok(LlmDelta::Output(text))) => {
                    events.emit(SessionEvent::Output { text });
                }
                Some(Ok(LlmDelta::ToolCall { name, .. })) => {
                    events.emit(SessionEvent::ToolCall { name });
                }
                Some(Err(llm_err)) => return interpret_chat_error(llm_err),
                None => break,
            }
        }

        match stream.take_response() {
            Some(Ok(response)) => response,
            Some(Err(llm_err)) => return interpret_chat_error(llm_err),
            None => return interpret_chat_error(claw_api::ChatError::truncated_stream()),
        }
    };

    #[cfg(feature = "cache_profile")]
    if let Some(usage) = llm_response.usage {
        tracing::info!(
            name: "usage",
            input_tokens = ?usage.input_tokens,
            output_tokens = ?usage.output_tokens,
            cache_read_tokens = ?usage.cache_read_tokens,
            cache_write_tokens = ?usage.cache_write_tokens,
        );
        events.emit(SessionEvent::Usage { usage });
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

    // Tool-call events were already emitted per call while streaming above.

    if let Err(err) = append_assistant_tool_calls(&mut appended, &llm_response) {
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
    }
}
