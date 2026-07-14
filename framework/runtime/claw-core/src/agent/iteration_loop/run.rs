use tracing::Instrument as _;

use claw_api::{ChatRequest, LlmDelta, ToolCall};
use claw_interface::http::StreamingHttp;
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_tool::ToolRunner;
use futures_lite::StreamExt;

use crate::event::{EventSink, SessionEvent, StreamPart};

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
    phase: ContentPhase,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ContentPhase {
    Reasoning,
    Output,
    ToolCalls,
    Ended,
}

impl<'a> IterationBracket<'a> {
    fn new(events: &'a EventSink) -> Self {
        Self {
            events,
            phase: ContentPhase::Reasoning,
        }
    }

    fn emit_reasoning_fragment(&mut self, fragment: &str, emitted: &mut usize) {
        debug_assert_eq!(self.phase, ContentPhase::Reasoning);
        self.events.emit_reasoning_fragment(fragment, emitted);
    }

    fn emit_output(&mut self, text: String) {
        self.finish_reasoning();
        debug_assert_eq!(self.phase, ContentPhase::Output);
        self.events
            .emit(SessionEvent::Output(StreamPart::Delta(text)));
    }

    fn emit_tool_call(&mut self, call: ToolCall) {
        self.finish_reasoning();
        self.finish_output();
        debug_assert_eq!(self.phase, ContentPhase::ToolCalls);
        self.events
            .emit(SessionEvent::ToolCalls(StreamPart::Delta(call)));
    }

    fn finish_content(&mut self) {
        self.finish_reasoning();
        self.finish_output();
        self.finish_tool_calls();
    }

    fn finish_reasoning(&mut self) {
        if self.phase == ContentPhase::Reasoning {
            self.events.emit(SessionEvent::Reasoning(StreamPart::End));
            self.phase = ContentPhase::Output;
        }
    }

    fn finish_output(&mut self) {
        if self.phase == ContentPhase::Output {
            self.events.emit(SessionEvent::Output(StreamPart::End));
            self.phase = ContentPhase::ToolCalls;
        }
    }

    fn finish_tool_calls(&mut self) {
        if self.phase == ContentPhase::ToolCalls {
            self.events.emit(SessionEvent::ToolCalls(StreamPart::End));
            self.phase = ContentPhase::Ended;
        }
    }
}

impl Drop for IterationBracket<'_> {
    fn drop(&mut self) {
        self.finish_content();
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
    let mut bracket = IterationBracket::new(events);
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
                    bracket.emit_reasoning_fragment(&text, &mut reasoning_emitted);
                }
                Some(Ok(LlmDelta::Output(text))) => {
                    bracket.emit_output(text);
                }
                Some(Ok(LlmDelta::ToolCall {
                    id,
                    name,
                    arguments,
                    ..
                })) => {
                    bracket.emit_tool_call(ToolCall {
                        id,
                        name,
                        arguments_json: arguments,
                    });
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
    bracket.finish_content();

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

#[cfg(test)]
mod tests {
    use claw_api::ToolCall;

    use crate::event::{EventSink, SessionEvent, StreamPart};

    use super::IterationBracket;

    #[test]
    fn iteration_bracket_closes_skipped_content_streams_in_order() {
        let (sender, receiver) = async_channel::unbounded();
        let events = EventSink::new(sender);
        {
            let mut bracket = IterationBracket::new(&events);
            let mut reasoning_emitted = 0;
            bracket.emit_reasoning_fragment("thinking", &mut reasoning_emitted);
            bracket.emit_tool_call(ToolCall {
                id: "call-1".to_string(),
                name: "search".to_string(),
                arguments_json: r#"{"query":"rust"}"#.to_string(),
            });
        }

        let mut actual = Vec::new();
        while let Ok(event) = receiver.try_recv() {
            actual.push(event);
        }
        assert_eq!(
            actual,
            vec![
                SessionEvent::Reasoning(StreamPart::Delta("thinking".to_string())),
                SessionEvent::Reasoning(StreamPart::End),
                SessionEvent::Output(StreamPart::End),
                SessionEvent::ToolCalls(StreamPart::Delta(ToolCall {
                    id: "call-1".to_string(),
                    name: "search".to_string(),
                    arguments_json: r#"{"query":"rust"}"#.to_string(),
                })),
                SessionEvent::ToolCalls(StreamPart::End),
                SessionEvent::IterationEnded,
            ]
        );
    }
}
