//! One LLM/tool round-trip per [`IterationLoop`].
//!
//! Layer 3 only: pass chat + a [`ToolSet`], LLM picks tool calls, we invoke
//! them, return which tools ran. No session, channel, or routing concepts here.
//!
//! On preemption this layer only detects the signal and ends the iteration.
//! It does not read, format, or return interrupt message content — upper layers
//! own pending input and context rebuild.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use serde_json::Value;
use strum::IntoStaticStr;
use tracing::Instrument as _;

use claw_api::{ChatError, ChatRequest, ClawApiAsync, LlmResponse, RetryPolicy};
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_tool::{
    ApprovalNeeded, RawToolInvocation, ToolGate, ToolInvocation, ToolRunOutcome, ToolRunner,
    ToolSetError, ToolSetHandle,
};

use crate::event::{EventSink, SessionEvent};

crate::define_prefixed_id!(IterationId, "iteration-", "iteration");

/// Emits [`SessionEvent::IterationEnded`] when dropped, so every `run_one_iteration`
/// exit path (plain answer, tool round, preempt, or error) closes the bracket its
/// [`SessionEvent::IterationStarted`] opened.
struct IterationBracket<'a> {
    events: &'a EventSink,
}

impl Drop for IterationBracket<'_> {
    fn drop(&mut self) {
        self.events.emit(SessionEvent::IterationEnded);
    }
}

/// Errors from one [`IterationLoop::run`] step.
#[derive(Clone, Debug, IntoStaticStr, thiserror::Error)]
pub(crate) enum IterationLoopError {
    #[strum(serialize = "messages_not_array")]
    #[error("messages must be a JSON array")]
    MessagesNotArray,
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

/// Borrowed OpenAI-style system prompt for one LLM call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct SystemPrompt<'a>(pub &'a str);

impl AsRef<str> for SystemPrompt<'_> {
    fn as_ref(&self) -> &str {
        self.0
    }
}

/// Borrowed OpenAI-style `messages` array for one LLM call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ChatMessages<'a>(pub &'a Value);

/// Owned message batch appended by one completed step (assistant/tool round).
#[derive(Clone, Debug, PartialEq, Default)]
pub(crate) struct AppendedMessages(pub Value);

impl AppendedMessages {
    pub(crate) fn empty() -> Self {
        Self(Value::Array(Vec::new()))
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.0.as_array().is_none_or(Vec::is_empty)
    }
}

/// Inputs for exactly one [`IterationLoop::run`]: chat fields + tools.
pub(crate) struct IterationStep<'a> {
    pub iteration_id: IterationId,
    pub system_prompt: SystemPrompt<'a>,
    pub messages: ChatMessages<'a>,
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

/// Terminal outcome of exactly one [`IterationLoop::run`] (completed or preempted).
#[derive(Clone, Debug)]
pub(crate) enum IterationOutcome {
    Completed(CompletedOutcome),
    Preempted(PreemptedOutcome),
}

/// One [`IterationLoop::run`] step: [`IterationOutcome`] or [`IterationLoopError`].
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
    disposition: ToolRunDisposition,
}

impl ToolRun {
    /// True when the call was refused by soft-hide gating (not in the tool set's
    /// active allow-set) instead of being invoked.
    pub(crate) fn is_blocked(&self) -> bool {
        matches!(self.disposition, ToolRunDisposition::Blocked)
    }

    pub(crate) fn approval(&self) -> Option<&ApprovalNeeded> {
        match &self.disposition {
            ToolRunDisposition::AwaitingApproval(approval) => Some(approval),
            ToolRunDisposition::Executed | ToolRunDisposition::Blocked => None,
        }
    }
}

/// Why a tool run did or did not execute. This keeps mutually exclusive
/// execution states in one field instead of pairing booleans with optional
/// payloads.
#[derive(Clone, Debug, PartialEq, Eq)]
enum ToolRunDisposition {
    Executed,
    Blocked,
    AwaitingApproval(ApprovalNeeded),
}

/// The model issued tool calls and they were executed.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ToolsOutcome {
    /// Assistant + tool messages produced this step (iteration layer merges).
    pub appended: AppendedMessages,
    pub runs: Vec<ToolRun>,
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
/// - [`IterationLoop`] consumes the flag via `swap(false)` when ending preempted.
pub(crate) trait InterruptionControl {
    /// Polled at checkpoints (consume) and passed to in-flight LLM HTTP (cooperative abort).
    fn interrupt_flag(&self) -> &Arc<AtomicBool>;
}

/// One-step executor: LLM + preempt control only. Tools live on [`IterationStep`].
///
/// Generic over the HTTP transport `H` so the LLM call stays statically
/// dispatched. The loop borrows the agent's [`ClawApiAsync`] mutably for exactly one
/// `chat` round, so it is consumed by [`run`](Self::run).
pub(crate) struct IterationLoop<'a, H: ClawHttp, Timer: ClawTimer> {
    pub llm: &'a mut ClawApiAsync<H, Timer>,
    pub interruption: &'a dyn InterruptionControl,
    /// Retry policy applied to this iteration's LLM call (see [`RetryPolicy`]).
    pub retry: RetryPolicy,
    /// Where this iteration's [`SessionEvent`]s are pushed. Disabled for subagents
    /// (and the internal approval resolver), so only the root's iteration events
    /// reach a submission's stream.
    pub events: &'a EventSink,
}

impl<H: ClawHttp, Timer: ClawTimer> IterationLoop<'_, H, Timer> {
    /// Execute exactly one iteration: LLM chat → optional tool execution.
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

enum ToolRoundResult {
    Completed { runs: Vec<ToolRun> },
    Preempted(PreemptedOutcome),
    Failed(IterationLoopError),
}

fn take_interrupt(interruption: &dyn InterruptionControl) -> bool {
    interruption.interrupt_flag().swap(false, Ordering::AcqRel)
}

fn check_preempt_at_checkpoint(
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

fn append_assistant_tool_calls(
    messages: &mut Value,
    response: &LlmResponse,
) -> Result<(), IterationLoopError> {
    let Some(raw) = response
        .raw_message_json
        .as_deref()
        .filter(|s| !s.is_empty())
    else {
        return Err(IterationLoopError::MissingAssistantMessage);
    };
    let Ok(assistant) = serde_json::from_str::<Value>(raw) else {
        return Err(IterationLoopError::MalformedAssistantMessage);
    };
    match messages.as_array_mut() {
        Some(a) => {
            a.push(assistant);
            Ok(())
        }
        None => Err(IterationLoopError::MessagesNotArray),
    }
}

async fn run_tool_calls(
    interruption: &dyn InterruptionControl,
    runner: &ToolRunner<'_>,
    appended: &mut AppendedMessages,
    response: &LlmResponse,
    iteration_id: IterationId,
) -> ToolRoundResult {
    if appended.0.as_array().is_none() {
        return ToolRoundResult::Failed(IterationLoopError::MessagesNotArray);
    }

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
                if let Err(error) = push_tool_message(appended, &tc.id, content, false) {
                    return ToolRoundResult::Failed(error);
                }
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

        if let Err(error) = push_tool_message(appended, &tc.id, content, ok) {
            return ToolRoundResult::Failed(error);
        }
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
) -> Result<(), IterationLoopError> {
    let tool_message = serde_json::json!({
        "role": "tool",
        "tool_call_id": id,
        "content": content,
        "is_error": !ok,
    });

    let Some(runtime_arr) = appended.0.as_array_mut() else {
        return Err(IterationLoopError::MessagesNotArray);
    };
    runtime_arr.push(tool_message);
    Ok(())
}
