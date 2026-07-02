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

use claw_api::{ChatError, ChatRequest, ClawApiAsync, ClawApiError, LlmResponse, RetryPolicy};
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_tool::{ApprovalNeeded, CallOutcome, ToolGate, ToolInvocation, ToolRunner, ToolSet};

use claw_utils::TruncatedText;

crate::define_prefixed_id!(IterationId, "iteration-", "iteration");

/// Errors from one [`IterationLoop::run`] step.
#[derive(Clone, Debug, thiserror::Error)]
pub enum IterationLoopError {
    #[error("messages must be a JSON array")]
    MessagesNotArray,
    #[error("LLM tool-call response missing raw assistant message JSON")]
    MissingAssistantMessage,
    #[error("LLM raw assistant message JSON is not valid JSON")]
    MalformedAssistantMessage,
    #[error("LLM issued tool calls but no tools port was supplied")]
    MissingTools,
    #[error(transparent)]
    Chat(#[from] ChatError),
}

/// Checkpoint where preemption was detected. The iteration is terminal at this point.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IterationCheckpoint {
    BeforeLlmHttp,
    InLlmHttpAbort,
    AfterLlmBeforeTool,
    BeforeTool,
}

/// Borrowed OpenAI-style system prompt for one LLM call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SystemPrompt<'a>(pub &'a str);

impl AsRef<str> for SystemPrompt<'_> {
    fn as_ref(&self) -> &str {
        self.0
    }
}

/// Borrowed OpenAI-style `messages` array for one LLM call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChatMessages<'a>(pub &'a Value);

/// Owned message batch appended by one completed step (assistant/tool round).
#[derive(Clone, Debug, PartialEq, Default)]
pub struct AppendedMessages(pub Value);

impl AppendedMessages {
    pub fn empty() -> Self {
        Self(Value::Array(Vec::new()))
    }

    fn as_produced_snapshot(&self) -> Option<AppendedMessages> {
        match self.0.as_array() {
            Some(items) if !items.is_empty() => Some(self.clone()),
            _ => None,
        }
    }
}

/// Inputs for exactly one [`IterationLoop::run`]: chat fields + optional tools.
pub struct IterationStep<'a> {
    pub iteration_id: IterationId,
    pub system_prompt: SystemPrompt<'a>,
    pub messages: ChatMessages<'a>,
    /// Ephemeral trailing messages for this request only (never persisted),
    /// appended after `messages`. Empty when there is nothing to nudge.
    pub reminders: &'a [Value],
    /// The tool set for this step. It also carries its own soft-hide allow-set
    /// (see [`ToolSet::set_active_tools`]): the full schema is always sent
    /// (cache-stable), but a call to a tool the set does not currently allow is
    /// refused with a tool error rather than invoked — see [`ToolRun::blocked`].
    pub tools: Option<&'a ToolSet>,
    /// Permission gate consulted before each call after soft-hide passes (`None`
    /// = no permission layer). On `Deny` the call is refused; on `Ask` it is held
    /// for human approval (surfaced via [`ToolRun::approval`]) and not run.
    pub gate: Option<&'a dyn ToolGate>,
}

/// Terminal outcome of exactly one [`IterationLoop::run`] (completed or preempted).
#[derive(Clone, Debug)]
pub enum IterationOutcome {
    Completed(CompletedOutcome),
    Preempted(PreemptedOutcome),
}

/// One [`IterationLoop::run`] step: [`IterationOutcome`] or [`IterationLoopError`].
pub type IterationResult = Result<IterationOutcome, IterationLoopError>;

/// Successful iteration: plain-text answer or executed tool round.
#[derive(Clone, Debug, PartialEq)]
pub struct CompletedOutcome {
    pub iteration_id: IterationId,
    pub kind: CompletedKind,
}

#[derive(Clone, Debug, PartialEq)]
pub enum CompletedKind {
    PlainText(PlainTextOutcome),
    Tools(ToolsOutcome),
}

/// One executed tool call (for iteration-level observers above this layer).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ToolRun {
    pub name: String,
    pub ok: bool,
    /// True when the call was refused by soft-hide gating (not in the tool set's
    /// active allow-set) instead of being invoked. A blocked run is always
    /// `ok == false`; upper layers use this to drive the "retry then fail"
    /// policy without treating it as a normal tool failure.
    pub blocked: bool,
    /// `Some` when the permission policy asked for human approval: the tool did
    /// not run and the agent layer raises + resolves the request. Crate-internal
    /// payload — observers outside the crate only see that approval is pending.
    pub(crate) approval: Option<ApprovalNeeded>,
}

/// The model issued tool calls and they were executed.
#[derive(Clone, Debug, PartialEq)]
pub struct ToolsOutcome {
    /// Assistant + tool messages produced this step (iteration layer merges).
    pub appended: AppendedMessages,
    pub runs: Vec<ToolRun>,
}

/// The model returned a final plain-text answer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlainTextOutcome {
    pub text: String,
    pub raw_message_json: Option<String>,
}

/// Iteration ended at a preempt checkpoint.
///
/// `produced` carries assistant/tool messages already materialized this step
/// (not interrupt message content). Upper layers decide whether to merge them.
#[derive(Clone, Debug, PartialEq)]
pub struct PreemptedOutcome {
    pub iteration_id: IterationId,
    pub checkpoint: IterationCheckpoint,
    pub produced: Option<AppendedMessages>,
}

/// User interrupt surface for one in-flight iteration. No message payloads here.
///
/// Contract with [`claw_interface::http::ClawHttp`]:
/// - Upper layer sets `interrupt_flag` to request cooperative abort.
/// - HTTP polls the flag and returns [`claw_interface::http::HttpError::Aborted`]
///   without clearing it (`claw_sys` / ESP HTTP keeps the flag intact).
/// - [`IterationLoop`] consumes the flag via `swap(false)` when ending preempted.
pub trait InterruptionControl {
    /// Polled at checkpoints (consume) and passed to in-flight LLM HTTP (cooperative abort).
    fn interrupt_flag(&self) -> &Arc<AtomicBool>;
}

/// One-step executor: LLM + preempt control only. Tools live on [`IterationStep`].
///
/// Generic over the HTTP transport `H` so the LLM call stays statically
/// dispatched. The loop borrows the agent's [`ClawApiAsync`] mutably for exactly one
/// `chat` round, so it is consumed by [`run`](Self::run).
pub struct IterationLoop<'a, H: ClawHttp, Timer: ClawTimer> {
    pub llm: &'a mut ClawApiAsync<H, Timer>,
    pub interruption: &'a dyn InterruptionControl,
    /// Retry policy applied to this iteration's LLM call (see [`RetryPolicy`]).
    pub retry: RetryPolicy,
}

impl<H: ClawHttp, Timer: ClawTimer> IterationLoop<'_, H, Timer> {
    /// Execute exactly one iteration: LLM chat → optional tool execution.
    pub async fn run(self, step: IterationStep<'_>) -> IterationResult {
        run_one_iteration(self, step).await
    }
}

async fn run_one_iteration<H: ClawHttp, Timer: ClawTimer>(
    loop_: IterationLoop<'_, H, Timer>,
    step: IterationStep<'_>,
) -> IterationResult {
    let iteration_id = step.iteration_id;
    // One span per iteration; tool-call spans nest beneath it.
    let _span =
        tracing::info_span!("iteration_loop", conversation.iteration = %iteration_id).entered();
    let mut appended = AppendedMessages::empty();

    if let Some(outcome) = check_preempt_at_checkpoint(
        loop_.interruption,
        iteration_id,
        IterationCheckpoint::BeforeLlmHttp,
        None,
    ) {
        return Ok(IterationOutcome::Preempted(outcome));
    }

    let chat_request = ChatRequest {
        system_prompt: step.system_prompt.as_ref(),
        messages: step.messages.0,
        reminders: step.reminders,
        tools_json: step.tools.and_then(|t| t.schemas_json()),
        retry: loop_.retry,
    };
    let cancel = Cancel::new(loop_.interruption.interrupt_flag().as_ref());
    let llm_response = match loop_.llm.chat(&chat_request, cancel).await {
        Ok(resp) => resp,
        Err(llm_err) => {
            if llm_http_preempted(loop_.interruption, &llm_err) {
                return Ok(IterationOutcome::Preempted(PreemptedOutcome {
                    iteration_id,
                    checkpoint: IterationCheckpoint::InLlmHttpAbort,
                    produced: None,
                }));
            }
            return Err(IterationLoopError::Chat(llm_err));
        }
    };

    if llm_response.tool_calls.is_empty() {
        if let Some(outcome) = check_preempt_at_checkpoint(
            loop_.interruption,
            iteration_id,
            IterationCheckpoint::AfterLlmBeforeTool,
            None,
        ) {
            return Ok(IterationOutcome::Preempted(outcome));
        }

        // Always emit a `toolcall` span so the trace keeps the consistent
        // turn > agent > iteration_loop > toolcall shape. With no tool call this
        // iteration, `tool=none` (and no `call_id`) marks the placeholder; a real
        // call carries `tool=<name>,call_id=<id>` — same `tool=` key either way.
        tracing::info_span!("toolcall", tool = "none").in_scope(|| {});

        let text = llm_response.text.clone().unwrap_or_default();
        // Free-form model text goes in the message slot (after ` | `, line end):
        // it may contain spaces/commas, which would break the `key=value` fields.
        tracing::info!(
            iteration = %iteration_id,
            status = "done",
            "{}",
            TruncatedText::new(&text)
        );
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
        None,
    ) {
        return Ok(IterationOutcome::Preempted(outcome));
    }

    log_tool_call_names(iteration_id, &llm_response);

    let Some(tools) = step.tools else {
        return Err(IterationLoopError::MissingTools);
    };

    if let Err(err) = append_assistant_tool_calls(&mut appended.0, &llm_response) {
        return Err(err);
    }

    let runner = ToolRunner::new(tools, step.gate);
    match run_tool_calls(
        loop_.interruption,
        &runner,
        &mut appended,
        &llm_response,
        iteration_id,
    )
    .await
    {
        ToolRoundResult::Completed { runs } => Ok(IterationOutcome::Completed(CompletedOutcome {
            iteration_id,
            kind: CompletedKind::Tools(ToolsOutcome { appended, runs }),
        })),
        ToolRoundResult::Preempted(outcome) => Ok(IterationOutcome::Preempted(outcome)),
        ToolRoundResult::Failed(err) => Err(err),
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

/// True when an in-flight LLM HTTP ended because of user interrupt.
fn llm_http_preempted(interruption: &dyn InterruptionControl, err: &ChatError) -> bool {
    if take_interrupt(interruption) {
        return true;
    }
    matches!(
        err,
        ChatError::Api(ClawApiError::Transport(msg)) if msg.contains("aborted")
    )
}

fn check_preempt_at_checkpoint(
    interruption: &dyn InterruptionControl,
    iteration_id: IterationId,
    checkpoint: IterationCheckpoint,
    produced: Option<AppendedMessages>,
) -> Option<PreemptedOutcome> {
    if !take_interrupt(interruption) {
        return None;
    }

    tracing::info!(iteration = %iteration_id, checkpoint = ?checkpoint, "iteration preempted");

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
        if let Some(outcome) = check_preempt_at_checkpoint(
            interruption,
            iteration_id,
            IterationCheckpoint::BeforeTool,
            appended.as_produced_snapshot(),
        ) {
            return ToolRoundResult::Preempted(outcome);
        }

        let display_name = if tc.name.is_empty() {
            "(null)"
        } else {
            tc.name.as_str()
        };
        // One span per tool call, covering gating + invoke + result. It lives
        // here (not in the runner / `ToolSet::invoke`) so a call refused by
        // soft-hide or permission gating — which never reaches `invoke` — is still
        // represented, and the "tool done" event below carries this span's `span=`.
        let _span =
            tracing::info_span!("toolcall", tool = display_name, call_id = %tc.id).entered();
        // Tool arguments are arbitrary JSON (spaces/commas) -> message slot.
        tracing::debug!("{}", TruncatedText::new(&tc.arguments_json));

        // The runner owns the decision (soft-hide -> permission -> execute); the
        // loop owns preemption, spans, message assembly. A matched tool message is
        // emitted for every call (even refused ones), so the patch stays
        // well-formed (no dangling tool_call ids).
        let CallOutcome {
            content,
            ok,
            blocked,
            approval,
        } = runner
            .run_one_async(&ToolInvocation {
                id: Some(&tc.id),
                name: &tc.name,
                arguments_json: &tc.arguments_json,
            })
            .await;
        // Tool output is free-form text -> message slot; keep ok/blocked as fields.
        tracing::info!(ok, blocked, "{}", TruncatedText::new(&content));

        let tool_message = serde_json::json!({
            "role": "tool",
            "tool_call_id": tc.id,
            "content": content,
            "is_error": !ok,
        });

        let Some(runtime_arr) = appended.0.as_array_mut() else {
            return ToolRoundResult::Failed(IterationLoopError::MessagesNotArray);
        };
        runtime_arr.push(tool_message);
        runs.push(ToolRun {
            name: if tc.name.is_empty() {
                "(null)".to_string()
            } else {
                tc.name.clone()
            },
            ok,
            blocked,
            approval,
        });
    }

    ToolRoundResult::Completed { runs }
}

fn log_tool_call_names(iteration_id: IterationId, response: &LlmResponse) {
    if response.tool_calls.is_empty() {
        return;
    }
    let names: Vec<&str> = response
        .tool_calls
        .iter()
        .map(|tc| {
            if tc.name.is_empty() {
                "(null)"
            } else {
                tc.name.as_str()
            }
        })
        .collect();
    tracing::debug!(
        iteration = %iteration_id,
        count = response.tool_calls.len(),
        names = %names.join(","),
        "llm tool calls"
    );
}

#[cfg(test)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use super::*;
    use claw_api::ToolCall;
    use claw_interface::StdThread;
    use claw_tool::{
        init_tool_executor, AllowedTools, Tool, ToolGroup, ToolHandler, ToolInvokeError, ToolOutput,
    };
    use serde_json::json;

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        init_tool_executor(StdThread).expect("tool executor");
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    struct FlagControl {
        interrupt: Arc<AtomicBool>,
    }

    impl FlagControl {
        fn new() -> Self {
            Self {
                interrupt: Arc::new(AtomicBool::new(false)),
            }
        }

        fn signal_interrupt(&self) {
            self.interrupt.store(true, Ordering::Release);
        }
    }

    impl InterruptionControl for FlagControl {
        fn interrupt_flag(&self) -> &Arc<AtomicBool> {
            &self.interrupt
        }
    }

    #[test]
    fn check_preempt_at_checkpoint_consumes_interrupt_flag() {
        let interruption = FlagControl::new();
        interruption.signal_interrupt();

        let outcome = check_preempt_at_checkpoint(
            &interruption,
            IterationId(1),
            IterationCheckpoint::AfterLlmBeforeTool,
            None,
        )
        .expect("preempt");

        assert_eq!(outcome.checkpoint, IterationCheckpoint::AfterLlmBeforeTool);
        assert!(outcome.produced.is_none());
        assert!(!interruption.interrupt_flag().load(Ordering::Acquire));
    }

    #[test]
    fn check_preempt_at_checkpoint_returns_produced_snapshot() {
        let interruption = FlagControl::new();
        interruption.signal_interrupt();

        let produced = AppendedMessages(json!([
            {"role": "assistant"},
            {"role": "tool", "content": "partial"}
        ]));
        let outcome = check_preempt_at_checkpoint(
            &interruption,
            IterationId(1),
            IterationCheckpoint::BeforeTool,
            produced.as_produced_snapshot(),
        )
        .expect("preempt");

        let produced = outcome.produced.expect("produced");
        let items = produced.0.as_array().expect("array");
        assert_eq!(items.len(), 2);
    }

    #[test]
    fn llm_http_preempted_accepts_transport_abort_without_flag() {
        let interruption = FlagControl::new();
        let err = ChatError::Api(ClawApiError::Transport(
            "HTTP transport error: HTTP request aborted by caller".into(),
        ));
        assert!(llm_http_preempted(&interruption, &err));
    }

    #[test]
    fn append_assistant_tool_calls_error_paths() {
        let mut messages = Value::Array(Vec::new());
        let missing_raw = LlmResponse {
            text: None,
            reasoning_content: None,
            raw_message_json: None,
            tool_calls: vec![ToolCall {
                id: "t1".into(),
                name: "files".into(),
                arguments_json: "{}".into(),
            }],
        };
        assert!(matches!(
            append_assistant_tool_calls(&mut messages, &missing_raw).unwrap_err(),
            IterationLoopError::MissingAssistantMessage
        ));

        let malformed = LlmResponse {
            text: None,
            reasoning_content: None,
            raw_message_json: Some("{not-json".into()),
            tool_calls: vec![ToolCall {
                id: "t1".into(),
                name: "files".into(),
                arguments_json: "{}".into(),
            }],
        };
        assert!(matches!(
            append_assistant_tool_calls(&mut messages, &malformed).unwrap_err(),
            IterationLoopError::MalformedAssistantMessage
        ));

        let valid = LlmResponse {
            text: None,
            reasoning_content: None,
            raw_message_json: Some(r#"{"role":"assistant"}"#.into()),
            tool_calls: vec![],
        };
        let mut not_array = Value::Object(Default::default());
        assert!(matches!(
            append_assistant_tool_calls(&mut not_array, &valid).unwrap_err(),
            IterationLoopError::MessagesNotArray
        ));
    }

    #[test]
    fn run_tool_calls_error_and_empty_name() {
        // The model emits an empty tool name; register a tool under that name so
        // dispatch lands and we exercise the "(null)" display + is_error path.
        struct FailingTool;
        impl ToolHandler for FailingTool {
            fn name(&self) -> &str {
                ""
            }

            fn schema(&self) -> &str {
                r#"{"type":"function","function":{"name":""}}"#
            }

            fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
                Ok(ToolOutput {
                    output: "done".into(),
                    ok: false,
                })
            }
        }

        let interruption = FlagControl::new();
        let tools = ToolSet::from_groups([ToolGroup::new("g", [Tool::new(FailingTool)])])
            .expect("tool set");
        let iteration_id = IterationId(1);
        let response = LlmResponse {
            text: None,
            reasoning_content: None,
            raw_message_json: None,
            tool_calls: vec![ToolCall {
                id: "t1".into(),
                name: String::new(),
                arguments_json: "{}".into(),
            }],
        };

        let runner = ToolRunner::new(&tools, None);
        let mut not_array = AppendedMessages(Value::Object(Default::default()));
        assert!(matches!(
            block_on(run_tool_calls(
                &interruption,
                &runner,
                &mut not_array,
                &response,
                iteration_id
            )),
            ToolRoundResult::Failed(IterationLoopError::MessagesNotArray)
        ));

        let mut appended = AppendedMessages::empty();
        append_assistant_tool_calls(
            &mut appended.0,
            &LlmResponse {
                text: None,
                reasoning_content: None,
                raw_message_json: Some(r#"{"role":"assistant"}"#.into()),
                tool_calls: vec![],
            },
        )
        .expect("assistant");

        let ToolRoundResult::Completed { runs } = block_on(run_tool_calls(
            &interruption,
            &runner,
            &mut appended,
            &response,
            iteration_id,
        )) else {
            panic!("expected completed tool round");
        };
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].name, "(null)");
        assert!(!runs[0].ok);
        assert!(!runs[0].blocked);
        assert_eq!(appended.0[1]["is_error"], true);
    }

    #[test]
    fn run_tool_calls_blocks_tool_not_in_allowed_set() {
        // A tool that would succeed if invoked; gating must refuse it instead.
        struct OkTool;
        impl ToolHandler for OkTool {
            fn name(&self) -> &str {
                "writer"
            }
            fn schema(&self) -> &str {
                r#"{"type":"function","function":{"name":"writer"}}"#
            }
            fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
                Ok(ToolOutput {
                    output: "wrote".into(),
                    ok: true,
                })
            }
        }

        let interruption = FlagControl::new();
        let mut tools =
            ToolSet::from_groups([ToolGroup::new("g", [Tool::new(OkTool)])]).expect("tool set");
        tools.set_active_tools(AllowedTools::new(["reader"])); // "writer" is not permitted
        let response = LlmResponse {
            text: None,
            reasoning_content: None,
            raw_message_json: None,
            tool_calls: vec![ToolCall {
                id: "t1".into(),
                name: "writer".into(),
                arguments_json: "{}".into(),
            }],
        };

        let mut appended = AppendedMessages::empty();
        append_assistant_tool_calls(
            &mut appended.0,
            &LlmResponse {
                text: None,
                reasoning_content: None,
                raw_message_json: Some(r#"{"role":"assistant"}"#.into()),
                tool_calls: vec![],
            },
        )
        .expect("assistant");

        let runner = ToolRunner::new(&tools, None);
        let ToolRoundResult::Completed { runs } = block_on(run_tool_calls(
            &interruption,
            &runner,
            &mut appended,
            &response,
            IterationId(1),
        )) else {
            panic!("expected completed tool round");
        };

        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].name, "writer");
        assert!(!runs[0].ok);
        assert!(runs[0].blocked);
        // The tool message is present (id matched, no dangling) and is an error.
        assert_eq!(appended.0[1]["tool_call_id"], "t1");
        assert_eq!(appended.0[1]["is_error"], true);
    }

    #[test]
    fn log_tool_call_names_handles_empty_and_null_names() {
        log_tool_call_names(
            IterationId(1),
            &LlmResponse {
                text: None,
                reasoning_content: None,
                raw_message_json: None,
                tool_calls: vec![],
            },
        );

        log_tool_call_names(
            IterationId(2),
            &LlmResponse {
                text: None,
                reasoning_content: None,
                raw_message_json: None,
                tool_calls: vec![
                    ToolCall {
                        id: "t1".into(),
                        name: String::new(),
                        arguments_json: "{}".into(),
                    },
                    ToolCall {
                        id: "t2".into(),
                        name: "files".into(),
                        arguments_json: "{}".into(),
                    },
                ],
            },
        );
    }

    #[test]
    fn iteration_id_serializes_to_prefixed_string() {
        let value = serde_json::to_value(IterationId(5)).unwrap();
        assert_eq!(value, serde_json::json!("iteration-5"));
    }

    #[test]
    fn iteration_id_deserializes_from_prefixed_string() {
        let iteration: IterationId =
            serde_json::from_value(serde_json::json!("iteration-4")).unwrap();
        assert_eq!(iteration, IterationId(4));
        assert_eq!(iteration.to_string(), "iteration-4");
    }

    #[test]
    fn append_assistant_tool_calls_accepts_valid_message() {
        let mut messages = Value::Array(Vec::new());
        let response = LlmResponse {
            text: None,
            reasoning_content: None,
            raw_message_json: Some(r#"{"role":"assistant","tool_calls":[]}"#.into()),
            tool_calls: vec![],
        };
        append_assistant_tool_calls(&mut messages, &response).expect("append assistant");
        assert_eq!(messages[0]["role"], "assistant");
    }
}

#[cfg(test)]
mod behavior_tests {
    //! Internal behavior tests for the iteration loop.

    use core::future::Future;
    use core::task::{Context, Poll};
    use std::collections::VecDeque;
    use std::path::Path;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};
    use std::task::{Wake, Waker};

    use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig, RetryPolicy};
    use claw_interface::http::{
        blocking::{ClawHttp as BlockingClawHttp, RealHttp},
        HttpError, HttpJsonRequest, HttpRequestFailure, HttpResponse, HttpStatusCode,
    };
    use claw_interface::{BlockingHttpAdapter, ClawHttp, ClawTimer, ImmediateTimer, StdThread};
    use claw_tool::{
        init_tool_executor, Tool, ToolError, ToolGroup, ToolHandler, ToolInvocation,
        ToolInvokeError, ToolOutput, ToolSet,
    };
    use serde_json::{json, Value};

    use super::{
        AppendedMessages, ChatMessages, CompletedKind, InterruptionControl, IterationCheckpoint,
        IterationId, IterationLoop, IterationLoopError, IterationOutcome, IterationResult,
        IterationStep, SystemPrompt, ToolsOutcome,
    };

    const PLAIN_TEXT_BODY: &str =
        r#"{"choices":[{"message":{"role":"assistant","content":"hello from model"}}]}"#;

    const TOOL_CALL_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"t1","function":{"name":"files","arguments":"{}"}}]}}]}"#;

    const TOOL_CALL_EMPTY_NAME_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"t1","function":{"name":"","arguments":"{}"}}]}}]}"#;

    type TestLlm<H> = ClawApiAsync<BlockingHttpAdapter<H>, ImmediateTimer>;

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        init_tool_executor(StdThread).expect("tool executor");
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    fn load_local_env() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join(".env.local");
        let _ = dotenvy::from_path(path);
    }

    fn local_env_path() -> std::path::PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join(".env.local")
    }

    fn require_local_api_key() -> String {
        let env_path = local_env_path();
        if !env_path.is_file() {
            panic!(
                "live test requires {} — copy .env.example to .env.local and set CLAW_LLM_API_KEY",
                env_path.display()
            );
        }

        load_local_env();
        std::env::var("CLAW_LLM_API_KEY")
            .ok()
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| {
                panic!(
                    "live test requires CLAW_LLM_API_KEY in {} (file exists but key is missing or empty)",
                    env_path.display()
                )
            })
    }

    fn local_base_url() -> String {
        load_local_env();
        std::env::var("CLAW_LLM_BASE_URL").unwrap_or_else(|_| "https://api.openai.com".into())
    }

    fn local_model() -> String {
        load_local_env();
        std::env::var("CLAW_LLM_MODEL").unwrap_or_else(|_| "gpt-4o-mini".into())
    }

    // The live transport for optional LLM tests is `claw_interface::RealHttp` (the
    // `realhttp` feature).

    struct ScriptedHttp {
        bodies: Mutex<VecDeque<String>>,
    }

    impl BlockingClawHttp for ScriptedHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            let body = self
                .bodies
                .lock()
                .unwrap()
                .pop_front()
                .unwrap_or_else(|| PLAIN_TEXT_BODY.into());
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body,
            })
        }
    }

    struct FailingHttp;

    impl BlockingClawHttp for FailingHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            Err(HttpError::RequestFailed(HttpRequestFailure::transport(
                "simulated transport failure",
            )))
        }
    }

    fn test_llm(bodies: Vec<&str>) -> TestLlm<ScriptedHttp> {
        let http = ScriptedHttp {
            bodies: Mutex::new(bodies.into_iter().map(str::to_string).collect()),
        };
        ClawApiAsync::init(
            ClawApiConfig::new(
                BackendKind::OpenAiCompatible,
                "sk-test",
                "gpt-test",
                "https://example.invalid",
            ),
            BlockingHttpAdapter::new(http),
            ImmediateTimer,
        )
        .expect("test llm init")
    }

    const TEST_ITERATION_ID: IterationId = IterationId(42);

    struct MockControl {
        interrupt_flag: Arc<AtomicBool>,
    }

    impl MockControl {
        fn new() -> Self {
            MockControl {
                interrupt_flag: Arc::new(AtomicBool::new(false)),
            }
        }

        fn signal_interrupt(&self) {
            self.interrupt_flag.store(true, Ordering::Release);
        }
    }

    impl InterruptionControl for MockControl {
        fn interrupt_flag(&self) -> &Arc<AtomicBool> {
            &self.interrupt_flag
        }
    }

    struct ArmInterruptAfterResponseHttp {
        bodies: Mutex<VecDeque<String>>,
        interrupt: Arc<AtomicBool>,
        arm: AtomicBool,
    }

    impl ArmInterruptAfterResponseHttp {
        fn new(interrupt: Arc<AtomicBool>, bodies: Vec<&str>) -> Self {
            Self {
                bodies: Mutex::new(bodies.into_iter().map(str::to_string).collect()),
                interrupt,
                arm: AtomicBool::new(false),
            }
        }

        fn arm(&self) {
            self.arm.store(true, Ordering::Release);
        }
    }

    impl BlockingClawHttp for ArmInterruptAfterResponseHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            let body = self
                .bodies
                .lock()
                .unwrap()
                .pop_front()
                .unwrap_or_else(|| PLAIN_TEXT_BODY.into());
            if self.arm.swap(false, Ordering::AcqRel) {
                self.interrupt.store(true, Ordering::Release);
            }
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body,
            })
        }
    }

    struct AbortDuringHttp {
        interrupt: Arc<AtomicBool>,
    }

    impl BlockingClawHttp for AbortDuringHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.interrupt.store(true, Ordering::Release);
            Err(HttpError::Aborted)
        }
    }

    /// Tool named `files` that echoes `name:args` and succeeds.
    struct EchoTool;

    impl ToolHandler for EchoTool {
        fn name(&self) -> &str {
            "files"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"files"}}"#
        }
        fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: format!("{}:{}", call.name, call.arguments_json),
                ok: true,
            })
        }
    }

    /// Tool named `files` whose invoke fails, to test error propagation.
    struct FailingTool;

    impl ToolHandler for FailingTool {
        fn name(&self) -> &str {
            "files"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"files"}}"#
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Err(ToolError::NotFound("files".into()).into())
        }
    }

    /// Tool registered under the empty name (matching an empty-name tool_call) that
    /// returns `ok: false`, to test the soft-fail / "(null)" display path.
    struct SoftFailTool;

    impl ToolHandler for SoftFailTool {
        fn name(&self) -> &str {
            ""
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":""}}"#
        }
        fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: format!("soft-fail:{}:{}", call.name, call.arguments_json),
                ok: false,
            })
        }
    }

    /// Wrap a single tool in a one-group [`ToolSet`] for tests.
    fn tool_set(tool: impl ToolHandler + 'static) -> ToolSet {
        ToolSet::from_groups([ToolGroup::new("test", [Tool::new(tool)])]).expect("tool set")
    }

    fn test_llm_with_http<H: BlockingClawHttp>(http: H) -> TestLlm<H> {
        ClawApiAsync::init(
            ClawApiConfig::new(
                BackendKind::OpenAiCompatible,
                "sk-test",
                "gpt-test",
                "https://example.invalid",
            ),
            BlockingHttpAdapter::new(http),
            ImmediateTimer,
        )
        .expect("test llm init")
    }

    fn run_step<H: ClawHttp, Timer: ClawTimer>(
        llm: &mut ClawApiAsync<H, Timer>,
        control: &MockControl,
        tools: Option<&ToolSet>,
        iteration_id: IterationId,
        messages: &Value,
        system_prompt: &str,
    ) -> IterationResult {
        let step = IterationStep {
            iteration_id,
            system_prompt: SystemPrompt(system_prompt),
            messages: ChatMessages(messages),
            reminders: &[],
            tools,
            gate: None,
        };
        block_on(
            IterationLoop {
                llm,
                interruption: control,
                retry: RetryPolicy::default(),
            }
            .run(step),
        )
    }

    #[test]
    fn appended_messages_empty_is_array() {
        let tail = AppendedMessages::empty();
        assert_eq!(tail.0, json!([]));
    }

    #[test]
    fn system_prompt_borrows_text() {
        let prompt = SystemPrompt("You are helpful.");
        assert_eq!(prompt.as_ref(), "You are helpful.");
    }

    #[test]
    fn run_returns_plain_text_outcome() {
        let mut llm = test_llm(vec![PLAIN_TEXT_BODY]);
        let control = MockControl::new();
        let messages = json!([{"role":"user","content":"hi"}]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let Ok(IterationOutcome::Completed(outcome)) = result else {
            panic!("expected Completed, got {result:?}");
        };
        match outcome.kind {
            CompletedKind::PlainText(text_outcome) => {
                assert_eq!(text_outcome.text, "hello from model");
                assert!(text_outcome.raw_message_json.is_some());
            }
            other => panic!("expected PlainText, got {other:?}"),
        }
    }

    #[test]
    fn run_executes_tools_and_records_runs() {
        let mut llm = test_llm(vec![TOOL_CALL_BODY]);
        let control = MockControl::new();
        let echo = tool_set(EchoTool);
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            Some(&echo),
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let Ok(IterationOutcome::Completed(outcome)) = result else {
            panic!("expected Completed, got {result:?}");
        };
        match outcome.kind {
            CompletedKind::Tools(tool_outcome) => {
                assert_eq!(tool_outcome.runs.len(), 1);
                assert_eq!(tool_outcome.runs[0].name, "files");
                assert!(tool_outcome.runs[0].ok);

                let appended = tool_outcome.appended.0.as_array().unwrap();
                assert_eq!(appended.len(), 2);
                assert_eq!(appended[0]["role"], "assistant");
                assert_eq!(appended[1]["role"], "tool");
                assert_eq!(appended[1]["tool_call_id"], "t1");
                assert_eq!(appended[1]["content"], "files:{}");
                assert_eq!(appended[1]["is_error"], false);
            }
            other => panic!("expected Tools, got {other:?}"),
        }
    }

    #[test]
    fn run_returns_preempted_when_interrupt_signaled_before_llm() {
        let mut llm = test_llm(vec![PLAIN_TEXT_BODY]);
        let control = MockControl::new();
        control.signal_interrupt();
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let Ok(IterationOutcome::Preempted(outcome)) = result else {
            panic!("expected Preempted, got {result:?}");
        };
        assert_eq!(outcome.checkpoint, IterationCheckpoint::BeforeLlmHttp);
        assert!(outcome.produced.is_none());
    }

    #[test]
    fn run_errors_when_tool_calls_without_tools_port() {
        let mut llm = test_llm(vec![TOOL_CALL_BODY]);
        let control = MockControl::new();
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        assert!(matches!(result, Err(IterationLoopError::MissingTools)));
    }

    #[test]
    fn run_returns_preempted_after_llm_before_tool_execution() {
        let control = MockControl::new();
        let http = ArmInterruptAfterResponseHttp::new(
            control.interrupt_flag().clone(),
            vec![TOOL_CALL_BODY],
        );
        http.arm();
        let mut llm = test_llm_with_http(http);
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let Ok(IterationOutcome::Preempted(outcome)) = result else {
            panic!("expected Preempted, got {result:?}");
        };
        assert_eq!(outcome.checkpoint, IterationCheckpoint::AfterLlmBeforeTool);
        assert!(outcome.produced.is_none());
    }

    #[test]
    fn run_returns_preempted_when_http_aborted() {
        let control = MockControl::new();
        let mut llm = test_llm_with_http(AbortDuringHttp {
            interrupt: control.interrupt_flag().clone(),
        });
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let Ok(IterationOutcome::Preempted(outcome)) = result else {
            panic!("expected Preempted, got {result:?}");
        };
        assert_eq!(outcome.checkpoint, IterationCheckpoint::InLlmHttpAbort);
        assert!(outcome.produced.is_none());
    }

    #[test]
    fn run_propagates_chat_errors_without_interrupt() {
        let mut llm = test_llm_with_http(FailingHttp);
        let control = MockControl::new();
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        assert!(matches!(result, Err(IterationLoopError::Chat(_))));
    }

    #[test]
    fn run_records_soft_failing_tool_with_null_name() {
        let mut llm = test_llm(vec![TOOL_CALL_EMPTY_NAME_BODY]);
        let control = MockControl::new();
        let soft_fail = tool_set(SoftFailTool);
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            Some(&soft_fail),
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let Ok(IterationOutcome::Completed(outcome)) = result else {
            panic!("expected Completed, got {result:?}");
        };
        match outcome.kind {
            CompletedKind::Tools(tool_outcome) => {
                assert_eq!(tool_outcome.runs.len(), 1);
                assert_eq!(tool_outcome.runs[0].name, "(null)");
                assert!(!tool_outcome.runs[0].ok);

                let appended = tool_outcome.appended.0.as_array().unwrap();
                assert_eq!(appended.len(), 2);
                assert_eq!(appended[1]["is_error"], true);
            }
            other => panic!("expected Tools, got {other:?}"),
        }
    }

    #[test]
    fn run_recovers_from_invoke_errors_as_tool_message() {
        let mut llm = test_llm(vec![TOOL_CALL_BODY]);
        let control = MockControl::new();
        let failing = tool_set(FailingTool);
        let messages = json!([]);

        let result = run_step(
            &mut llm,
            &control,
            Some(&failing),
            TEST_ITERATION_ID,
            &messages,
            "system",
        );

        let outcome = result.expect("invoke errors become tool messages");
        match outcome {
            IterationOutcome::Completed(completed) => {
                let ToolsOutcome { appended, runs } = match completed.kind {
                    CompletedKind::Tools(tools) => tools,
                    other => panic!("expected Tools, got {other:?}"),
                };
                assert_eq!(runs.len(), 1);
                assert!(!runs[0].ok);
                let messages = appended.0.as_array().expect("messages array");
                assert_eq!(messages.len(), 2);
                assert!(messages[1]["content"]
                    .as_str()
                    .unwrap()
                    .contains("not registered"));
                assert_eq!(messages[1]["is_error"], true);
            }
            other => panic!("expected Completed, got {other:?}"),
        }
    }

    #[test]
    fn live_plain_text_when_api_key_configured() {
        let api_key = require_local_api_key();

        let http = RealHttp::new();
        let mut llm = ClawApiAsync::init(
            {
                let mut config = ClawApiConfig::new(
                    BackendKind::OpenAiCompatible,
                    api_key,
                    local_model(),
                    local_base_url(),
                );
                config.timeout_ms = 60_000;
                config
            },
            BlockingHttpAdapter::new(http),
            ImmediateTimer,
        )
        .expect("live llm init");

        let control = MockControl::new();
        let messages = json!([{"role":"user","content":"Reply with exactly: pong"}]);

        let result = run_step(
            &mut llm,
            &control,
            None,
            TEST_ITERATION_ID,
            &messages,
            "You are a test assistant. Be brief.",
        );

        let Ok(IterationOutcome::Completed(outcome)) = result else {
            panic!("expected Completed from live model, got {result:?}");
        };
        match outcome.kind {
            CompletedKind::PlainText(text_outcome) => {
                assert!(
                    text_outcome.text.to_lowercase().contains("pong"),
                    "unexpected model text: {}",
                    text_outcome.text
                );
            }
            other => panic!("expected PlainText from live model, got {other:?}"),
        }
    }
}
