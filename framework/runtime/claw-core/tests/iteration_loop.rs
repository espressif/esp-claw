//! Public API tests for [`claw_core::iteration_loop`].

use std::collections::VecDeque;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use claw_api::{ClawApi, ClawApiConfig, RetryPolicy};
use claw_core::iteration_loop::{
    AppendedMessages, ChatMessages, CompletedKind, InterruptionControl, IterationCheckpoint,
    IterationLoop, IterationLoopError, IterationOutcome, IterationResult, IterationStep,
    SystemPrompt, ToolsOutcome,
};
use claw_core::{
    IterationId, Tool, ToolError, ToolGroup, ToolHandler, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolSet,
};
use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
use claw_interface::RealHttp;
use serde_json::{json, Value};

const PLAIN_TEXT_BODY: &str =
    r#"{"choices":[{"message":{"role":"assistant","content":"hello from model"}}]}"#;

const TOOL_CALL_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"t1","function":{"name":"files","arguments":"{}"}}]}}]}"#;

const TOOL_CALL_EMPTY_NAME_BODY: &str = r#"{"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"t1","function":{"name":"","arguments":"{}"}}]}}]}"#;

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

impl ClawHttp for ScriptedHttp {
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
            status_code: 200,
            body,
        })
    }
}

struct FailingHttp;

impl ClawHttp for FailingHttp {
    fn post_json(
        &mut self,
        _request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        Err(HttpError::RequestFailed(
            "simulated transport failure".into(),
        ))
    }
}

fn test_llm(bodies: Vec<&str>) -> ClawApi<ScriptedHttp> {
    let http = ScriptedHttp {
        bodies: Mutex::new(bodies.into_iter().map(str::to_string).collect()),
    };
    ClawApi::init(
        ClawApiConfig {
            api_key: Some("sk-test".into()),
            backend_type: "openai_compatible".into(),
            model: Some("gpt-test".into()),
            base_url: Some("https://example.invalid".into()),
            supports_tools: true,
            ..Default::default()
        },
        http,
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

impl ClawHttp for ArmInterruptAfterResponseHttp {
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
            status_code: 200,
            body,
        })
    }
}

struct AbortDuringHttp {
    interrupt: Arc<AtomicBool>,
}

impl ClawHttp for AbortDuringHttp {
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

fn test_llm_with_http<H: ClawHttp>(http: H) -> ClawApi<H> {
    ClawApi::init(
        ClawApiConfig {
            api_key: Some("sk-test".into()),
            backend_type: "openai_compatible".into(),
            model: Some("gpt-test".into()),
            base_url: Some("https://example.invalid".into()),
            supports_tools: true,
            ..Default::default()
        },
        http,
    )
    .expect("test llm init")
}

fn run_step<H: ClawHttp>(
    llm: &mut ClawApi<H>,
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
    IterationLoop {
        llm,
        interruption: control,
        retry: RetryPolicy::default(),
    }
    .run(step)
}

#[test]
fn appended_messages_empty_and_extend() {
    let mut tail = AppendedMessages::empty();
    assert_eq!(tail.0, json!([]));

    let mut extra = AppendedMessages(json!([{"role":"user","content":"more"}]));
    tail.extend(&extra);
    assert_eq!(tail.0.as_array().unwrap().len(), 1);

    extra.extend(&AppendedMessages::empty());
    assert_eq!(extra.0, json!([{"role":"user","content":"more"}]));

    let mut non_array_dest = AppendedMessages(json!({"not": "array"}));
    non_array_dest.extend(&AppendedMessages(json!([{"role":"user","content":"x"}])));
    assert_eq!(non_array_dest.0, json!({"not": "array"}));

    let mut empty = AppendedMessages::empty();
    empty.extend(&AppendedMessages(json!("not-an-array")));
    assert!(empty.0.as_array().unwrap().is_empty());
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
    let http =
        ArmInterruptAfterResponseHttp::new(control.interrupt_flag().clone(), vec![TOOL_CALL_BODY]);
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
    let mut llm = ClawApi::init(
        ClawApiConfig {
            api_key: Some(api_key),
            backend_type: "openai_compatible".into(),
            model: Some(local_model()),
            base_url: Some(local_base_url()),
            supports_tools: true,
            timeout_ms: 60_000,
            ..Default::default()
        },
        http,
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
