//! `claw-api` — LLM client: OpenAI-/Anthropic-compatible chat, structured JSON
//! output, and image inference over an injected HTTP transport.
//!
//! Extracted from `claw_core::llm` into a standalone crate so the LLM client
//! surface can be reused independently of the agent core (e.g. by
//! `claw_memory`'s async extractor and `cap_llm_inspect`).
//!
//! # Overview
//!
//! The entry point is [`ClawApi`]. You build it once from a [`ClawApiConfig`]
//! plus an HTTP transport (any [`claw_interface::http::ClawHttp`]), then issue
//! requests:
//!
//! | Method | Request | Returns |
//! |---|---|---|
//! | [`ClawApi::chat`] | [`ChatRequest`] | [`LlmResponse`] (text + tool calls) |
//! | [`ClawApi::chat_json`] | [`ChatJsonRequest`] | [`ChatJsonResponse`] (parsed `T` + tool calls) |
//! | [`ClawApi::infer_media`] | [`MediaRequest`] | `String` (model text about the image) |
//!
//! Networking is **injected**: `claw-api` never opens sockets itself. On device
//! the espidf layer implements [`ClawHttp`](claw_interface::http::ClawHttp) over
//! `esp_http_client`; tests and host tools provide their own implementation.
//!
//! # Cancellation
//!
//! Every call takes `&AtomicBool` abort flag. Set it from another thread to
//! cancel: the transport stops the in-flight request and any retry backoff sleep
//! returns early. An aborted call surfaces as a non-retryable
//! [`ClawApiError::Transport`] whose message contains `"aborted"`.
//!
//! # Retries
//!
//! Retry is configured **per call** via [`RetryPolicy`] on the request (not on
//! the client). A freshly constructed request carries [`RetryPolicy::default`]
//! (2 retries, 500ms initial interval, exponential, capped at 8s); override it
//! with `.with_retry(...)`, or disable retry with [`RetryPolicy::none`]. Only
//! transient transport failures are retried (network errors and HTTP
//! 408/429/5xx); aborts, bad URLs/bodies, and other 4xx are never retried. See
//! [`RetryPolicy`] for the knobs and [`ClawApiError::is_retryable`] for the
//! classification.
//!
//! # End-to-end example
//!
//! ```no_run
//! use std::sync::atomic::AtomicBool;
//! use claw_api::{ChatRequest, ClawApi, ClawApiConfig, RetryPolicy};
//! use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
//!
//! // 1. Provide an HTTP transport. On device this wraps `esp_http_client`;
//! //    here we stub a fixed OpenAI-shaped reply.
//! struct MyHttp;
//! impl ClawHttp for MyHttp {
//!     fn post_json(&mut self, _req: &HttpJsonRequest, _abort: &AtomicBool)
//!         -> Result<HttpResponse, HttpError> {
//!         Ok(HttpResponse {
//!             status_code: 200,
//!             body: r#"{"choices":[{"message":{"role":"assistant","content":"Hi!"}}]}"#.into(),
//!         })
//!     }
//! }
//!
//! // 2. Build the client once. It owns the transport and is driven via `&mut`.
//! let config = ClawApiConfig {
//!     api_key: Some("sk-...".into()),
//!     backend_type: "openai_compatible".into(), // or "anthropic_compatible"
//!     model: Some("gpt-4o-mini".into()),
//!     base_url: Some("https://api.openai.com/v1".into()),
//!     supports_tools: true,
//!     supports_vision: true,
//!     ..Default::default()
//! };
//! let mut api = ClawApi::init(config, MyHttp)?;
//!
//! // 3. Chat. The abort flag can be flipped from another thread to cancel.
//! let messages = serde_json::json!([{ "role": "user", "content": "Hello" }]);
//! let abort = AtomicBool::new(false);
//! let reply = api.chat(
//!     &ChatRequest::new("You are a helpful assistant.", &messages)
//!         .with_retry(RetryPolicy::new(3)), // optional: override default retry
//!     &abort,
//! )?;
//! assert_eq!(reply.text.as_deref(), Some("Hi!"));
//! # Ok::<(), anyhow::Error>(())
//! ```

// Implementation modules are private: the public surface is the curated
// re-exports below. The backend registry, media-prep pipeline, prompt helpers,
// and retry loop are internal details, not part of the end-user API.
mod backend;
mod backends;
mod client;
mod errors;
mod json_output;
mod media;
mod retry;
mod types;

pub use client::ClawApi;
pub use errors::{ChatError, ChatJsonError, ClawApiError, InferMediaError, InitError};
pub use types::{
    AssetKind, ChatJsonRequest, ChatJsonResponse, ChatRequest, ClawApiConfig, LlmResponse,
    MediaAsset, MediaRequest, ModelProfile, RetryPolicy, StaticOutputSchema, ToolCall,
    DEFAULT_BACKOFF_MULTIPLIER, DEFAULT_MAX_BACKOFF_MS, DEFAULT_MAX_RETRIES,
    DEFAULT_RETRY_INTERVAL_MS,
};

#[cfg(test)]
mod tests {
    use super::{
        ChatError, ChatRequest, ClawApi, ClawApiConfig, ClawApiError, InitError, RetryPolicy,
    };
    use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    use core::sync::atomic::AtomicBool;
    use serde_json::{json, Value};
    use std::sync::{Arc, Mutex};

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

    /// `ClawApi<H>` owns its transport by value. The doubles below record into
    /// interior-mutable fields and are shared with the test via `Arc`, so the
    /// test wraps a cloned `Arc` in this newtype to hand ownership to the client
    /// while keeping its own handle to assert on what was sent.
    struct Owned<T>(Arc<T>);

    impl ClawHttp for Owned<MockHttp> {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            *self.0.last_body.lock().unwrap() = Some(request.body.to_string());
            *self.0.last_url.lock().unwrap() = Some(request.url.to_string());
            Ok(HttpResponse {
                status_code: 200,
                body: self.0.reply.clone(),
            })
        }
    }

    fn cfg(backend: &str, base_url: &str) -> ClawApiConfig {
        ClawApiConfig {
            api_key: Some("key".into()),
            backend_type: backend.into(),
            model: Some("model-x".into()),
            base_url: Some(base_url.into()),
            supports_tools: true,
            supports_vision: true,
            ..Default::default()
        }
    }

    #[test]
    fn openai_chat_text() {
        let http =
            MockHttp::new(r#"{"choices":[{"message":{"role":"assistant","content":"hi there"}}]}"#);
        let mut rt = ClawApi::init(
            cfg("openai_compatible", "https://api.example.com/v1"),
            Owned(http.clone()),
        )
        .unwrap();
        let messages = json!([{"role": "user", "content": "hello"}]);
        let abort = AtomicBool::new(false);
        let resp = rt
            .chat(&ChatRequest::new("sys", &messages), &abort)
            .unwrap();
        assert_eq!(resp.text.as_deref(), Some("hi there"));

        // URL joined with one slash; body carries system + user messages.
        assert_eq!(
            http.last_url.lock().unwrap().as_deref(),
            Some("https://api.example.com/v1/chat/completions")
        );
        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs[0]["role"], "system");
        assert_eq!(msgs[0]["content"], "sys");
        assert_eq!(msgs[1]["role"], "user");
        assert_eq!(body["model"], "model-x");
    }

    #[test]
    fn openai_tool_calls_parsed() {
        let reply = r#"{"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[
            {"id":"call_1","function":{"name":"files","arguments":"{\"p\":\"/x\"}"}}]}}]}"#;
        let http = MockHttp::new(reply);
        let mut rt = ClawApi::init(
            cfg("openai_compatible", "https://api.example.com"),
            Owned(http),
        )
        .unwrap();
        let messages = json!([{"role": "user", "content": "list"}]);
        let abort = AtomicBool::new(false);
        let resp = rt.chat(&ChatRequest::new("s", &messages), &abort).unwrap();
        assert_eq!(resp.tool_calls.len(), 1);
        assert_eq!(resp.tool_calls[0].id, "call_1");
        assert_eq!(resp.tool_calls[0].name, "files");
        assert_eq!(resp.tool_calls[0].arguments_json, r#"{"p":"/x"}"#);
    }

    #[test]
    fn anthropic_converts_tool_role_to_user_and_parses() {
        let reply = r#"{"content":[{"type":"thinking","thinking":"hmm"},{"type":"text","text":"done"},
            {"type":"tool_use","id":"tu1","name":"foo","input":{"a":1}}]}"#;
        let http = MockHttp::new(reply);
        let mut rt = ClawApi::init(
            cfg("anthropic_compatible", "https://api.anthropic.com/v1"),
            Owned(http.clone()),
        )
        .unwrap();

        // assistant with tool_calls, then a tool result message
        let messages = json!([
            {"role": "user", "content": "hi"},
            {"role": "assistant", "content": "", "tool_calls": [
                {"id": "tu1", "function": {"name": "foo", "arguments": "{\"a\":1}"}}
            ]},
            {"role": "tool", "tool_call_id": "tu1", "content": "result-text"}
        ]);
        let abort = AtomicBool::new(false);
        let resp = rt
            .chat(&ChatRequest::new("sys", &messages), &abort)
            .unwrap();
        assert_eq!(resp.text.as_deref(), Some("done"));
        assert_eq!(resp.reasoning_content.as_deref(), Some("hmm"));
        assert_eq!(resp.tool_calls.len(), 1);
        assert_eq!(resp.tool_calls[0].name, "foo");

        // Verify the request conversion: tool role becomes a user message with a
        // tool_result block; assistant carries a tool_use block.
        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
        assert_eq!(body["system"], "sys");
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs[0]["role"], "user");
        let assistant = &msgs[1];
        assert_eq!(assistant["role"], "assistant");
        let a_blocks = assistant["content"].as_array().unwrap();
        assert!(a_blocks
            .iter()
            .any(|b| b["type"] == "tool_use" && b["name"] == "foo"));
        let tool_user = &msgs[2];
        assert_eq!(tool_user["role"], "user");
        let t_blocks = tool_user["content"].as_array().unwrap();
        assert_eq!(t_blocks[0]["type"], "tool_result");
        assert_eq!(t_blocks[0]["tool_use_id"], "tu1");
        assert_eq!(t_blocks[0]["content"], "result-text");
    }

    #[test]
    fn anthropic_converts_tools() {
        let http = MockHttp::new(r#"{"content":[{"type":"text","text":"ok"}]}"#);
        let mut rt = ClawApi::init(
            cfg("anthropic_compatible", "https://api.anthropic.com"),
            Owned(http.clone()),
        )
        .unwrap();
        let messages = json!([{"role": "user", "content": "hi"}]);
        let tools = r#"[{"type":"function","function":{"name":"foo","description":"d","parameters":{"type":"object"}}}]"#;
        let abort = AtomicBool::new(false);
        rt.chat(&ChatRequest::new("s", &messages).with_tools(tools), &abort)
            .unwrap();
        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
        let tools_out = body["tools"].as_array().unwrap();
        assert_eq!(tools_out[0]["name"], "foo");
        assert_eq!(tools_out[0]["description"], "d");
        assert_eq!(tools_out[0]["input_schema"]["type"], "object");
        assert_eq!(body["tool_choice"]["type"], "auto");
    }

    #[test]
    fn unknown_backend_rejected() {
        let http = MockHttp::new("{}");
        let err = match ClawApi::init(cfg("nope", "https://x"), Owned(http)) {
            Ok(_) => panic!("expected error"),
            Err(e) => e,
        };
        assert!(matches!(err, InitError::UnknownBackend));
    }

    #[derive(Debug, serde::Deserialize)]
    struct DemoOut {
        action: String,
        value: u32,
    }

    fn demo_req(messages: &Value) -> super::ChatJsonRequest<'_> {
        super::ChatJsonRequest::new("sys", messages).with_output_schema("demo_out", DEMO_OUT_SCHEMA)
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
            cfg("openai_compatible", "https://api.example.com/v1"),
            Owned(http.clone()),
        )
        .unwrap();
        assert!(rt.profile().supports_json_schema);

        let messages = json!([{"role": "user", "content": "go"}]);
        let abort = AtomicBool::new(false);
        let out = rt
            .chat_json::<DemoOut>(&demo_req(&messages), &abort)
            .unwrap();
        assert_eq!(out.output.as_ref().unwrap().action, "ok");
        assert_eq!(out.output.as_ref().unwrap().value, 7);

        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
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
            cfg("anthropic_compatible", "https://api.anthropic.com/v1"),
            Owned(http.clone()),
        )
        .unwrap();
        assert!(rt.profile().supports_json_schema);

        let messages = json!([{"role": "user", "content": "go"}]);
        let abort = AtomicBool::new(false);
        let out = rt
            .chat_json::<DemoOut>(&demo_req(&messages), &abort)
            .unwrap();
        assert_eq!(out.output.as_ref().unwrap().value, 3);

        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
        assert_eq!(body["output_config"]["format"]["type"], "json_schema");
        assert_eq!(body["output_config"]["format"]["schema"]["type"], "object");
        assert_eq!(body["system"], "sys");
        assert!(body.get("tools").is_none());
    }

    #[test]
    fn anthropic_chat_json_uses_prompt_fallback() {
        let reply = r#"{"content":[{"type":"text","text":"{\"action\":\"ok\",\"value\":3}"}]}"#;
        let http = MockHttp::new(reply);
        let mut config = cfg("anthropic_compatible", "https://api.anthropic.com/v1");
        config.supports_json_schema = Some(false);
        let mut rt = ClawApi::init(config, Owned(http.clone())).unwrap();
        assert!(!rt.profile().supports_json_schema);

        let messages = json!([{"role": "user", "content": "go"}]);
        let abort = AtomicBool::new(false);
        let out = rt
            .chat_json::<DemoOut>(&demo_req(&messages), &abort)
            .unwrap();
        assert_eq!(out.output.as_ref().unwrap().value, 3);

        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
        assert!(body.get("output_config").is_none());
        let system = body["system"].as_str().unwrap();
        assert!(system.contains("Respond with a single JSON object"));
        assert!(system.contains("\"demo_out\"") || system.contains("action"));
    }

    #[test]
    fn anthropic_chat_json_sends_tools_with_output_config() {
        let reply = r#"{"content":[{"type":"text","text":"{\"action\":\"ok\",\"value\":5}"}]}"#;
        let http = MockHttp::new(reply);
        let mut rt = ClawApi::init(
            cfg("anthropic_compatible", "https://api.anthropic.com/v1"),
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

        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
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
            cfg("openai_compatible", "https://api.example.com"),
            Owned(http),
        )
        .unwrap();
        let messages = json!([{"role": "user", "content": "go"}]);
        let abort = AtomicBool::new(false);
        let err = rt
            .chat_json::<DemoOut>(&demo_req(&messages), &abort)
            .unwrap_err();
        assert!(matches!(err, super::ChatJsonError::InvalidOutput(_)));
    }

    #[test]
    fn openai_chat_json_sends_tools_with_response_format() {
        let reply = r#"{"choices":[{"message":{"role":"assistant","content":"{\"action\":\"ok\",\"value\":1}"}}]}"#;
        let http = MockHttp::new(reply);
        let mut rt = ClawApi::init(
            cfg("openai_compatible", "https://api.example.com/v1"),
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

        let body: Value =
            serde_json::from_str(http.last_body.lock().unwrap().as_deref().unwrap()).unwrap();
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
            cfg("openai_compatible", "https://api.example.com"),
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

    /// Fails the first `fail_count` calls with `error`, then returns 200 + `reply`.
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

    impl ClawHttp for Owned<FlakyHttp> {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            *self.0.calls.lock().unwrap() += 1;
            let mut remaining = self.0.remaining_failures.lock().unwrap();
            if *remaining > 0 {
                *remaining -= 1;
                return Err(self.0.error.clone());
            }
            Ok(HttpResponse {
                status_code: 200,
                body: self.0.reply.clone(),
            })
        }
    }

    /// Zero-backoff policy so retry tests don't actually sleep.
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
        // First retry waits the configured interval.
        assert_eq!(p.backoff_ms(1), 500);
        assert_eq!(RetryPolicy::default().initial_backoff_ms, 500);

        // Custom interval via builder.
        let custom = RetryPolicy::new(2).with_interval_ms(1500);
        assert_eq!(custom.backoff_ms(1), 1500);

        // Fixed interval: same wait on every retry.
        let fixed = RetryPolicy::fixed(3, 250);
        assert_eq!(fixed.backoff_ms(1), 250);
        assert_eq!(fixed.backoff_ms(2), 250);
        assert_eq!(fixed.backoff_ms(3), 250);
    }

    fn flaky_rt(http: Arc<FlakyHttp>) -> ClawApi<Owned<FlakyHttp>> {
        ClawApi::init(
            cfg("openai_compatible", "https://api.example.com"),
            Owned(http),
        )
        .unwrap()
    }

    #[test]
    fn retry_succeeds_after_transient_failures() {
        let http = FlakyHttp::new(
            2,
            HttpError::RequestFailed("connection reset".into()),
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
        assert_eq!(*http.calls.lock().unwrap(), 3);
    }

    #[test]
    fn retry_exhausts_and_returns_transient_error() {
        let http = FlakyHttp::new(9, HttpError::RequestFailed("down".into()), "{}");
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
        // first attempt + 2 retries
        assert_eq!(*http.calls.lock().unwrap(), 3);
    }

    #[test]
    fn retry_skips_non_retryable_status() {
        let http = FlakyHttp::new(
            9,
            HttpError::UnexpectedStatus("HTTP 401: bad key".into()),
            "{}",
        );
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
        assert_eq!(*http.calls.lock().unwrap(), 1);
    }

    #[test]
    fn retry_retries_server_error_status() {
        let http = FlakyHttp::new(
            1,
            HttpError::UnexpectedStatus("HTTP 503: try later".into()),
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
        assert_eq!(*http.calls.lock().unwrap(), 2);
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
        // Abort maps to a permanent Transport error containing "aborted".
        assert!(matches!(
            &err,
            ChatError::Api(ClawApiError::Transport(msg)) if msg.contains("aborted")
        ));
        assert_eq!(*http.calls.lock().unwrap(), 1);
    }

    #[test]
    fn default_policy_applies_when_retry_unset() {
        // No `.with_retry`: `ChatRequest::new` defaults `retry` to
        // RetryPolicy::default(), which retries transient failures
        // (max_retries=2). Pre-abort so the test does not actually sleep.
        let http = FlakyHttp::new(2, HttpError::RequestFailed("blip".into()), "{}");
        let mut rt = flaky_rt(http.clone());
        let messages = json!([{"role": "user", "content": "hi"}]);
        let abort = AtomicBool::new(true); // pre-aborted: skip backoff sleeps

        // Pre-aborted means the first transient failure stops at the backoff
        // sleep, so we observe exactly one attempt — proving default retry is
        // wired (the loop entered) without sleeping.
        let err = rt
            .chat(&ChatRequest::new("s", &messages), &abort)
            .unwrap_err();
        assert!(matches!(
            &err,
            ChatError::Api(ClawApiError::Transport(msg)) if msg.contains("aborted")
        ));
        assert_eq!(*http.calls.lock().unwrap(), 1);
        assert_eq!(RetryPolicy::default().max_retries, 2);
    }
}
