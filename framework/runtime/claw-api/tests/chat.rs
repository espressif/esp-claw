use core::sync::atomic::AtomicBool;
use std::cell::RefCell;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use claw_api::{
    BackendKind, ChatError, ChatJsonError, ChatJsonRequest, ChatRequest, ClawApi, ClawApiAsync,
    ClawApiConfig, ClawApiError, InitError, RetryPolicy,
};
use claw_interface::http::blocking::ClawHttp as BlockingClawHttp;
use claw_interface::{
    Cancel, ClawHttp, ClawTimer, HttpError, HttpJsonRequest, HttpRequestFailure, HttpResponse,
    HttpResponseFuture, HttpStatusCode, ImmediateTimer, SleepOutcome, TimerFuture,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};

#[derive(Debug)]
struct TestFailure(String);

impl std::fmt::Display for TestFailure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for TestFailure {}

impl From<InitError> for TestFailure {
    fn from(error: InitError) -> Self {
        Self(error.to_string())
    }
}

impl From<ChatError> for TestFailure {
    fn from(error: ChatError) -> Self {
        Self(error.to_string())
    }
}

impl From<ChatJsonError> for TestFailure {
    fn from(error: ChatJsonError) -> Self {
        Self(error.to_string())
    }
}

impl From<serde_json::Error> for TestFailure {
    fn from(error: serde_json::Error) -> Self {
        Self(error.to_string())
    }
}

type TestResult = Result<(), TestFailure>;

fn fail(message: impl Into<String>) -> TestFailure {
    TestFailure(message.into())
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

struct MockHttp {
    reply: String,
    last_body: Mutex<Option<String>>,
    last_url: Mutex<Option<String>>,
}

impl MockHttp {
    fn new(reply: &str) -> Arc<Self> {
        Arc::new(MockHttp {
            reply: reply.to_string(),
            last_body: Mutex::new(None),
            last_url: Mutex::new(None),
        })
    }
}

struct Owned<T>(Arc<T>);

thread_local! {
    static DEFAULT_MOCK_HTTP: RefCell<Option<Arc<MockHttp>>> = const { RefCell::new(None) };
}

fn install_mock_http(http: Arc<MockHttp>) {
    DEFAULT_MOCK_HTTP.with(|slot| *slot.borrow_mut() = Some(http));
}

impl Default for Owned<MockHttp> {
    fn default() -> Self {
        DEFAULT_MOCK_HTTP.with(|slot| {
            Self(
                slot.borrow()
                    .as_ref()
                    .expect("install mock http before init_default")
                    .clone(),
            )
        })
    }
}

impl BlockingClawHttp for Owned<MockHttp> {
    fn post_json(
        &mut self,
        request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        *lock(&self.0.last_body) = Some(request.body.to_string());
        *lock(&self.0.last_url) = Some(request.url.to_string());
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: self.0.reply.clone(),
        })
    }
}

impl<T> ClawHttp for Owned<T>
where
    Owned<T>: BlockingClawHttp,
{
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            let never = AtomicBool::new(false);
            let result = BlockingClawHttp::post_json(self, request, &never);
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            result
        })
    }
}

fn cfg(backend: BackendKind, base_url: &str) -> ClawApiConfig {
    ClawApiConfig::new(backend, "key", "model-x", base_url)
}

fn captured_body(http: &MockHttp) -> Result<Value, TestFailure> {
    let body = lock(&http.last_body);
    let body = body
        .as_deref()
        .ok_or_else(|| fail("missing captured request body"))?;
    Ok(serde_json::from_str(body)?)
}

fn field<'a>(value: &'a Value, key: &str) -> Result<&'a Value, TestFailure> {
    value
        .get(key)
        .ok_or_else(|| fail(format!("missing JSON field `{key}`")))
}

fn array_field<'a>(value: &'a Value, key: &str) -> Result<&'a [Value], TestFailure> {
    field(value, key)?
        .as_array()
        .map(Vec::as_slice)
        .ok_or_else(|| fail(format!("JSON field `{key}` is not an array")))
}

fn array_item(items: &[Value], index: usize) -> Result<&Value, TestFailure> {
    items
        .get(index)
        .ok_or_else(|| fail(format!("missing JSON array item `{index}`")))
}

fn must_err<T, E>(result: Result<T, E>, context: &str) -> Result<E, TestFailure> {
    match result {
        Ok(_) => Err(fail(context)),
        Err(error) => Ok(error),
    }
}

#[test]
fn openai_chat_text() -> TestResult {
    let http =
        MockHttp::new(r#"{"choices":[{"message":{"role":"assistant","content":"hi there"}}]}"#);
    let mut rt = ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com/v1"),
        Owned(http.clone()),
    )?;
    let messages = json!([{"role": "user", "content": "hello"}]);
    let abort = AtomicBool::new(false);
    let resp = rt.chat(&ChatRequest::new("sys", &messages), &abort)?;
    assert_eq!(resp.text.as_deref(), Some("hi there"));

    assert_eq!(
        lock(&http.last_url).as_deref(),
        Some("https://api.example.com/v1/chat/completions")
    );
    let body = captured_body(&http)?;
    let msgs = array_field(&body, "messages")?;
    assert_eq!(field(array_item(msgs, 0)?, "role")?, "system");
    assert_eq!(field(array_item(msgs, 0)?, "content")?, "sys");
    assert_eq!(field(array_item(msgs, 1)?, "role")?, "user");
    assert_eq!(field(&body, "model")?, "model-x");
    Ok(())
}

#[test]
fn async_openai_chat_text() -> TestResult {
    let http =
        MockHttp::new(r#"{"choices":[{"message":{"role":"assistant","content":"hi async"}}]}"#);
    install_mock_http(Arc::clone(&http));
    let mut rt = ClawApiAsync::<Owned<MockHttp>, ImmediateTimer>::init_default(cfg(
        BackendKind::OpenAiCompatible,
        "https://api.example.com/v1",
    ))?;
    let messages = json!([{"role": "user", "content": "hello"}]);
    let abort = AtomicBool::new(false);

    let resp = block_on(rt.chat(&ChatRequest::new("sys", &messages), Cancel::new(&abort)))?;

    assert_eq!(resp.text.as_deref(), Some("hi async"));
    assert_eq!(
        lock(&http.last_url).as_deref(),
        Some("https://api.example.com/v1/chat/completions")
    );
    Ok(())
}

#[test]
fn openai_tool_calls_parsed() -> TestResult {
    let reply = r#"{"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[
        {"id":"call_1","function":{"name":"files","arguments":"{\"p\":\"/x\"}"}}]}}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com"),
        Owned(http),
    )?;
    let messages = json!([{"role": "user", "content": "list"}]);
    let abort = AtomicBool::new(false);
    let resp = rt.chat(&ChatRequest::new("s", &messages), &abort)?;
    assert_eq!(resp.tool_calls.len(), 1);
    let tool_call = resp
        .tool_calls
        .first()
        .ok_or_else(|| fail("missing tool call"))?;
    assert_eq!(tool_call.id, "call_1");
    assert_eq!(tool_call.name, "files");
    assert_eq!(tool_call.arguments_json, r#"{"p":"/x"}"#);
    Ok(())
}

#[test]
fn anthropic_converts_tool_role_to_user_and_parses() -> TestResult {
    let reply = r#"{"content":[{"type":"thinking","thinking":"hmm"},{"type":"text","text":"done"},
        {"type":"tool_use","id":"tu1","name":"foo","input":{"a":1}}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(
            BackendKind::AnthropicCompatible,
            "https://api.anthropic.com/v1",
        ),
        Owned(http.clone()),
    )?;

    let messages = json!([
        {"role": "user", "content": "hi"},
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "tu1", "function": {"name": "foo", "arguments": "{\"a\":1}"}}
        ]},
        {"role": "tool", "tool_call_id": "tu1", "content": "result-text"}
    ]);
    let abort = AtomicBool::new(false);
    let resp = rt.chat(&ChatRequest::new("sys", &messages), &abort)?;
    assert_eq!(resp.text.as_deref(), Some("done"));
    assert_eq!(resp.reasoning_content.as_deref(), Some("hmm"));
    assert_eq!(resp.tool_calls.len(), 1);
    let tool_call = resp
        .tool_calls
        .first()
        .ok_or_else(|| fail("missing parsed tool call"))?;
    assert_eq!(tool_call.name, "foo");

    let body = captured_body(&http)?;
    assert_eq!(field(&body, "system")?, "sys");
    let msgs = array_field(&body, "messages")?;
    assert_eq!(field(array_item(msgs, 0)?, "role")?, "user");
    let assistant = array_item(msgs, 1)?;
    assert_eq!(field(assistant, "role")?, "assistant");
    let a_blocks = array_field(assistant, "content")?;
    assert!(a_blocks
        .iter()
        .any(
            |b| field(b, "type").ok().and_then(Value::as_str) == Some("tool_use")
                && field(b, "name").ok().and_then(Value::as_str) == Some("foo")
        ));
    let tool_user = array_item(msgs, 2)?;
    assert_eq!(field(tool_user, "role")?, "user");
    let t_blocks = array_field(tool_user, "content")?;
    let tool_result = array_item(t_blocks, 0)?;
    assert_eq!(field(tool_result, "type")?, "tool_result");
    assert_eq!(field(tool_result, "tool_use_id")?, "tu1");
    assert_eq!(field(tool_result, "content")?, "result-text");
    Ok(())
}

#[test]
fn anthropic_converts_tools() -> TestResult {
    let http = MockHttp::new(r#"{"content":[{"type":"text","text":"ok"}]}"#);
    let mut rt = ClawApi::init(
        cfg(
            BackendKind::AnthropicCompatible,
            "https://api.anthropic.com",
        ),
        Owned(http.clone()),
    )?;
    let messages = json!([{"role": "user", "content": "hi"}]);
    let tools = r#"[{"type":"function","function":{"name":"foo","description":"d","parameters":{"type":"object"}}}]"#;
    let abort = AtomicBool::new(false);
    rt.chat(&ChatRequest::new("s", &messages).with_tools(tools), &abort)?;
    let body = captured_body(&http)?;
    let tools_out = array_field(&body, "tools")?;
    let tool = array_item(tools_out, 0)?;
    assert_eq!(field(tool, "name")?, "foo");
    assert_eq!(field(tool, "description")?, "d");
    assert_eq!(field(field(tool, "input_schema")?, "type")?, "object");
    assert_eq!(field(field(&body, "tool_choice")?, "type")?, "auto");
    Ok(())
}

#[test]
fn backend_parse_rejects_unknown_and_config_rejects_empty_key() -> TestResult {
    assert!("nope".parse::<BackendKind>().is_err());

    let http = MockHttp::new("{}");
    let err = must_err(
        ClawApi::init(
            ClawApiConfig::new(BackendKind::OpenAiCompatible, "", "model-x", "https://x"),
            Owned(http),
        ),
        "expected missing API key error",
    )?;
    assert!(matches!(err, InitError::MissingApiKey));
    Ok(())
}

#[derive(Debug, serde::Deserialize)]
struct DemoOut {
    action: String,
    value: u32,
}

fn demo_req(messages: &Value) -> ChatJsonRequest<'_> {
    ChatJsonRequest::new("sys", messages).with_output_schema("demo_out", DEMO_OUT_SCHEMA)
}

const DEMO_OUT_SCHEMA: &str = r#"{
    "type": "object",
    "properties": {
        "action": { "type": "string" },
        "value": { "type": "integer" }
    },
    "required": ["action", "value"],
    "additionalProperties": false
}"#;

#[test]
fn openai_chat_json_uses_response_format() {
    let reply = r#"{"choices":[{"message":{"role":"assistant","content":"{\"action\":\"ok\",\"value\":7}"}}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com/v1"),
        Owned(http.clone()),
    )
    .unwrap();

    let messages = json!([{"role": "user", "content": "go"}]);
    let abort = AtomicBool::new(false);
    let out = rt
        .chat_json::<DemoOut>(&demo_req(&messages), &abort)
        .unwrap();
    assert_eq!(out.output.as_ref().unwrap().action, "ok");
    assert_eq!(out.output.as_ref().unwrap().value, 7);

    let body: Value = serde_json::from_str(lock(&http.last_body).as_deref().unwrap()).unwrap();
    assert_eq!(body["response_format"]["type"], "json_schema");
    assert_eq!(body["response_format"]["json_schema"]["name"], "demo_out");
    assert_eq!(body["response_format"]["json_schema"]["strict"], true);
    assert!(body.get("tools").is_none());
}

#[test]
fn anthropic_chat_json_uses_output_config() {
    let reply = r#"{"content":[{"type":"text","text":"{\"action\":\"ok\",\"value\":3}"}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(
            BackendKind::AnthropicCompatible,
            "https://api.anthropic.com/v1",
        ),
        Owned(http.clone()),
    )
    .unwrap();

    let messages = json!([{"role": "user", "content": "go"}]);
    let abort = AtomicBool::new(false);
    let out = rt
        .chat_json::<DemoOut>(&demo_req(&messages), &abort)
        .unwrap();
    assert_eq!(out.output.as_ref().unwrap().value, 3);

    let body: Value = serde_json::from_str(lock(&http.last_body).as_deref().unwrap()).unwrap();
    assert_eq!(body["output_config"]["format"]["type"], "json_schema");
    assert_eq!(body["output_config"]["format"]["schema"]["type"], "object");
    assert_eq!(body["system"], "sys");
    assert!(body.get("tools").is_none());
}

#[test]
fn anthropic_chat_json_sends_tools_with_output_config() {
    let reply = r#"{"content":[{"type":"text","text":"{\"action\":\"ok\",\"value\":5}"}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(
            BackendKind::AnthropicCompatible,
            "https://api.anthropic.com/v1",
        ),
        Owned(http.clone()),
    )
    .unwrap();
    let messages = json!([{"role": "user", "content": "go"}]);
    let tools = r#"[{"type":"function","function":{"name":"files","description":"d","parameters":{"type":"object"}}}]"#;
    let abort = AtomicBool::new(false);

    let out = rt
        .chat_json::<DemoOut>(&demo_req(&messages).with_tools(tools), &abort)
        .unwrap();
    assert_eq!(out.output.as_ref().unwrap().value, 5);

    let body: Value = serde_json::from_str(lock(&http.last_body).as_deref().unwrap()).unwrap();
    assert_eq!(body["output_config"]["format"]["type"], "json_schema");
    assert_eq!(body["tools"].as_array().unwrap().len(), 1);
    assert_eq!(body["tools"][0]["name"], "files");
    assert_eq!(body["tools"][0]["strict"], true);
}

#[test]
fn chat_json_rejects_invalid_output() {
    let reply = r#"{"choices":[{"message":{"role":"assistant","content":"not-json"}}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com"),
        Owned(http),
    )
    .unwrap();
    let messages = json!([{"role": "user", "content": "go"}]);
    let abort = AtomicBool::new(false);
    let err = rt
        .chat_json::<DemoOut>(&demo_req(&messages), &abort)
        .unwrap_err();
    assert!(matches!(err, ChatJsonError::InvalidOutput(_)));
}

#[test]
fn openai_chat_json_sends_tools_with_response_format() {
    let reply = r#"{"choices":[{"message":{"role":"assistant","content":"{\"action\":\"ok\",\"value\":1}"}}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com/v1"),
        Owned(http.clone()),
    )
    .unwrap();
    let messages = json!([{"role": "user", "content": "go"}]);
    let tools = r#"[{"type":"function","function":{"name":"files","description":"d","parameters":{"type":"object"}}}]"#;
    let abort = AtomicBool::new(false);

    let out = rt
        .chat_json::<DemoOut>(&demo_req(&messages).with_tools(tools), &abort)
        .unwrap();
    assert_eq!(out.output.as_ref().unwrap().value, 1);

    let body: Value = serde_json::from_str(lock(&http.last_body).as_deref().unwrap()).unwrap();
    assert_eq!(body["response_format"]["type"], "json_schema");
    assert_eq!(body["tools"].as_array().unwrap().len(), 1);
    assert_eq!(body["tools"][0]["function"]["name"], "files");
}

#[test]
fn chat_json_returns_tool_calls_without_json() {
    let reply = r#"{"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[
        {"id":"call_1","function":{"name":"files","arguments":"{}"}}]}}]}"#;
    let http = MockHttp::new(reply);
    let mut rt = ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com"),
        Owned(http),
    )
    .unwrap();
    let messages = json!([{"role": "user", "content": "go"}]);
    let tools = r#"[{"type":"function","function":{"name":"files","description":"d","parameters":{"type":"object"}}}]"#;
    let abort = AtomicBool::new(false);

    let out = rt
        .chat_json::<DemoOut>(&demo_req(&messages).with_tools(tools), &abort)
        .unwrap();
    assert!(out.output.is_none());
    assert_eq!(out.tool_calls.len(), 1);
    assert_eq!(out.tool_calls[0].name, "files");
}

struct FlakyHttp {
    remaining_failures: Mutex<u32>,
    error: HttpError,
    reply: String,
    calls: Mutex<u32>,
}

impl FlakyHttp {
    fn new(fail_count: u32, error: HttpError, reply: &str) -> Arc<Self> {
        Arc::new(FlakyHttp {
            remaining_failures: Mutex::new(fail_count),
            error,
            reply: reply.to_string(),
            calls: Mutex::new(0),
        })
    }
}

thread_local! {
    static DEFAULT_FLAKY_HTTP: RefCell<Option<Arc<FlakyHttp>>> = const { RefCell::new(None) };
}

fn install_flaky_http(http: Arc<FlakyHttp>) {
    DEFAULT_FLAKY_HTTP.with(|slot| *slot.borrow_mut() = Some(http));
}

impl Default for Owned<FlakyHttp> {
    fn default() -> Self {
        DEFAULT_FLAKY_HTTP.with(|slot| {
            Self(
                slot.borrow()
                    .as_ref()
                    .expect("install flaky http before init_default")
                    .clone(),
            )
        })
    }
}

impl BlockingClawHttp for Owned<FlakyHttp> {
    fn post_json(
        &mut self,
        _request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        *lock(&self.0.calls) += 1;
        let mut remaining = lock(&self.0.remaining_failures);
        if *remaining > 0 {
            *remaining -= 1;
            return Err(self.0.error.clone());
        }
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: self.0.reply.clone(),
        })
    }
}

#[derive(Clone)]
struct RecordingTimer {
    sleeps: Arc<Mutex<Vec<Duration>>>,
}

thread_local! {
    static DEFAULT_RECORDING_SLEEPS: RefCell<Option<Arc<Mutex<Vec<Duration>>>>> =
        const { RefCell::new(None) };
}

fn install_recording_timer() -> Arc<Mutex<Vec<Duration>>> {
    let sleeps = Arc::new(Mutex::new(Vec::new()));
    DEFAULT_RECORDING_SLEEPS.with(|slot| *slot.borrow_mut() = Some(Arc::clone(&sleeps)));
    sleeps
}

impl Default for RecordingTimer {
    fn default() -> Self {
        DEFAULT_RECORDING_SLEEPS.with(|slot| Self {
            sleeps: slot
                .borrow()
                .as_ref()
                .expect("install recording timer before init_default")
                .clone(),
        })
    }
}

impl ClawTimer for RecordingTimer {
    fn sleep<'a>(&'a mut self, duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
        let sleeps = Arc::clone(&self.sleeps);
        Box::pin(async move {
            lock(&sleeps).push(duration);
            if cancel.is_cancelled() {
                SleepOutcome::Cancelled
            } else {
                SleepOutcome::Completed
            }
        })
    }
}

fn transport_error(message: &str) -> HttpError {
    HttpError::RequestFailed(HttpRequestFailure::transport(message))
}

fn unexpected_status(status: u16, message: &str) -> HttpError {
    HttpError::UnexpectedStatus {
        status: HttpStatusCode::new(status),
        message: message.to_string(),
    }
}

fn instant_retry(max_retries: u32) -> RetryPolicy {
    RetryPolicy::new(max_retries)
        .with_interval_ms(0)
        .with_max_backoff_ms(0)
}

#[test]
fn retry_policy_constructors_default_500ms_interval() {
    let p = RetryPolicy::new(3);
    assert_eq!(p.max_retries, 3);
    assert_eq!(p.initial_backoff_ms, 500);
    assert_eq!(p.backoff_ms(1), 500);
    assert_eq!(RetryPolicy::default().initial_backoff_ms, 500);

    let custom = RetryPolicy::new(2).with_interval_ms(1500);
    assert_eq!(custom.backoff_ms(1), 1500);

    let fixed = RetryPolicy::fixed(3, 250);
    assert_eq!(fixed.backoff_ms(1), 250);
    assert_eq!(fixed.backoff_ms(2), 250);
    assert_eq!(fixed.backoff_ms(3), 250);
}

fn flaky_rt(http: Arc<FlakyHttp>) -> ClawApi<Owned<FlakyHttp>> {
    ClawApi::init(
        cfg(BackendKind::OpenAiCompatible, "https://api.example.com"),
        Owned(http),
    )
    .unwrap()
}

#[test]
fn retry_succeeds_after_transient_failures() {
    let http = FlakyHttp::new(
        2,
        transport_error("connection reset"),
        r#"{"choices":[{"message":{"role":"assistant","content":"ok"}}]}"#,
    );
    let mut rt = flaky_rt(http.clone());
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(false);

    let resp = rt
        .chat(
            &ChatRequest::new("s", &messages).with_retry(instant_retry(3)),
            &abort,
        )
        .unwrap();
    assert_eq!(resp.text.as_deref(), Some("ok"));
    assert_eq!(*lock(&http.calls), 3);
}

#[test]
fn async_retry_uses_timer_backoff() {
    let http = FlakyHttp::new(
        2,
        transport_error("connection reset"),
        r#"{"choices":[{"message":{"role":"assistant","content":"ok"}}]}"#,
    );
    install_flaky_http(Arc::clone(&http));
    let sleeps = install_recording_timer();
    let mut rt = ClawApiAsync::<Owned<FlakyHttp>, RecordingTimer>::init_default(cfg(
        BackendKind::OpenAiCompatible,
        "https://api.example.com",
    ))
    .unwrap();
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(false);

    let resp = block_on(rt.chat(
        &ChatRequest::new("s", &messages).with_retry(RetryPolicy::fixed(3, 250)),
        Cancel::new(&abort),
    ))
    .unwrap();

    assert_eq!(resp.text.as_deref(), Some("ok"));
    assert_eq!(*lock(&http.calls), 3);
    assert_eq!(
        lock(&sleeps).as_slice(),
        &[Duration::from_millis(250), Duration::from_millis(250)]
    );
}

#[test]
fn retry_exhausts_and_returns_transient_error() {
    let http = FlakyHttp::new(9, transport_error("down"), "{}");
    let mut rt = flaky_rt(http.clone());
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(false);

    let err = rt
        .chat(
            &ChatRequest::new("s", &messages).with_retry(instant_retry(2)),
            &abort,
        )
        .unwrap_err();
    assert!(matches!(
        err,
        ChatError::Api(ClawApiError::TransientTransport(_))
    ));
    assert_eq!(*lock(&http.calls), 3);
}

#[test]
fn retry_skips_non_retryable_status() {
    let http = FlakyHttp::new(9, unexpected_status(401, "HTTP 401: bad key"), "{}");
    let mut rt = flaky_rt(http.clone());
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(false);

    let err = rt
        .chat(
            &ChatRequest::new("s", &messages).with_retry(instant_retry(5)),
            &abort,
        )
        .unwrap_err();
    assert!(matches!(err, ChatError::Api(ClawApiError::Transport(_))));
    assert_eq!(*lock(&http.calls), 1);
}

#[test]
fn retry_retries_server_error_status() {
    let http = FlakyHttp::new(
        1,
        unexpected_status(503, "HTTP 503: try later"),
        r#"{"choices":[{"message":{"role":"assistant","content":"recovered"}}]}"#,
    );
    let mut rt = flaky_rt(http.clone());
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(false);

    let resp = rt
        .chat(
            &ChatRequest::new("s", &messages).with_retry(instant_retry(3)),
            &abort,
        )
        .unwrap();
    assert_eq!(resp.text.as_deref(), Some("recovered"));
    assert_eq!(*lock(&http.calls), 2);
}

#[test]
fn abort_is_not_retried() {
    let http = FlakyHttp::new(9, HttpError::Aborted, "{}");
    let mut rt = flaky_rt(http.clone());
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(false);

    let err = rt
        .chat(
            &ChatRequest::new("s", &messages).with_retry(instant_retry(5)),
            &abort,
        )
        .unwrap_err();
    assert!(matches!(
        &err,
        ChatError::Api(ClawApiError::Transport(msg)) if msg.contains("aborted")
    ));
    assert_eq!(*lock(&http.calls), 1);
}

#[test]
fn default_policy_applies_when_retry_unset() {
    let http = FlakyHttp::new(2, transport_error("blip"), "{}");
    let mut rt = flaky_rt(http.clone());
    let messages = json!([{"role": "user", "content": "hi"}]);
    let abort = AtomicBool::new(true);

    let err = rt
        .chat(&ChatRequest::new("s", &messages), &abort)
        .unwrap_err();
    assert!(matches!(
        &err,
        ChatError::Api(ClawApiError::Transport(msg)) if msg.contains("aborted")
    ));
    assert_eq!(*lock(&http.calls), 1);
    assert_eq!(RetryPolicy::default().max_retries, 2);
}
