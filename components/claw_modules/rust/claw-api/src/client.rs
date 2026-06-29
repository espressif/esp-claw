//! `ClawApi` — the LLM client, port of `claw_llm_runtime.c`.
//!
//! Holds the resolved config, the derived model profile, the constructed
//! backend, and the injected [`ClawHttp`]. [`ClawApi::init`] applies the same
//! defaulting logic as the C `claw_llm_runtime_init`.

use core::sync::atomic::AtomicBool;

use serde::de::DeserializeOwned;
use serde_json::Value;

use claw_interface::http::ClawHttp;

use super::backend::{find_builtin_registration, LlmBackend};
use super::backends::{anthropic, openai_compatible};
use super::errors::{ChatError, ChatJsonError, ClawApiError, InferMediaError, InitError};
use super::retry::run_with_retry;
use super::types::{
    ChatJsonRequest, ChatJsonResponse, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest,
    ModelProfile,
};

/// Message used when the abort flag fires during a retry backoff sleep. Kept
/// containing "aborted" so upstream string-based abort detection still matches.
const ABORTED_DURING_BACKOFF: &str = "LLM request aborted during retry backoff";

const DEFAULT_TIMEOUT_MS: u32 = 120 * 1000;
const DEFAULT_MAX_TOKENS: u32 = 8192;
const DEFAULT_IMAGE_MAX_BYTES: usize = 512 * 1024;

/// The LLM client: a resolved backend + model profile behind an injected
/// [`ClawHttp`] transport.
///
/// Construct it once with [`ClawApi::init`], then reuse it for many requests.
///
/// Generic over the concrete transport `H` (static dispatch): the client *owns*
/// its `H` and drives it through `&mut self`, so a transport may keep and reuse a
/// persistent connection handle (keep-alive) across calls. There is no
/// `Arc<dyn ClawHttp>` and no built-in synchronization — the `Send`/`Sync` auto
/// traits flow from `H`, so threading requirements are decided by the caller at
/// the point of sharing (e.g. `Mutex<ClawApi<H>>` for a transport shared across
/// tasks), not imposed by this type.
///
/// # Example
///
/// ```no_run
/// use std::sync::atomic::AtomicBool;
/// use claw_api::{ChatRequest, ClawApi, ClawApiConfig};
/// # use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
/// # struct H; impl ClawHttp for H {
/// #   fn post_json(&mut self, _r: &HttpJsonRequest, _a: &AtomicBool) -> Result<HttpResponse, HttpError> {
/// #     Ok(HttpResponse { status_code: 200, body: r#"{"choices":[{"message":{"role":"assistant","content":"ok"}}]}"#.into() }) } }
/// let mut api = ClawApi::init(
///     ClawApiConfig {
///         api_key: Some("sk-...".into()),
///         backend_type: "openai_compatible".into(),
///         model: Some("gpt-4o-mini".into()),
///         base_url: Some("https://api.openai.com/v1".into()),
///         ..Default::default()
///     },
///     H,
/// )?;
/// let msgs = serde_json::json!([{ "role": "user", "content": "hi" }]);
/// let abort = AtomicBool::new(false);
/// let resp = api.chat(&ChatRequest::new("sys", &msgs), &abort)?;
/// # Ok::<(), anyhow::Error>(())
/// ```
pub struct ClawApi<H: ClawHttp> {
    profile: ModelProfile,
    backend: Box<dyn LlmBackend<H>>,
    http: H,
}

fn empty(value: &Option<String>) -> bool {
    value.as_deref().map(|s| s.is_empty()).unwrap_or(true)
}

impl<H: ClawHttp> ClawApi<H> {
    /// Validate `config`, select the built-in backend, and bind the `http`
    /// transport. (Port of `claw_llm_runtime_init`.)
    ///
    /// Missing fields are filled with backend defaults: `auth_type`,
    /// `chat_path`, `max_tokens_field`, a 120s `timeout_ms`, 8192 `max_tokens`,
    /// and a 512KiB `image_max_bytes`. `supports_json_schema = None` enables
    /// API-level JSON schema for backends that support it (`openai_compatible`,
    /// `anthropic_compatible`).
    ///
    /// `backend_type` must be one of `"openai_compatible"` or
    /// `"anthropic_compatible"`.
    ///
    /// # Errors
    ///
    /// Returns [`InitError`] when `api_key`, `model`, or `backend_type` is empty,
    /// or `backend_type` is unknown.
    ///
    /// # Example
    ///
    /// ```
    /// use std::sync::atomic::AtomicBool;
    /// use claw_api::{ClawApi, ClawApiConfig, InitError};
    /// # use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    /// # struct H; impl ClawHttp for H {
    /// #   fn post_json(&mut self, _r: &HttpJsonRequest, _a: &AtomicBool) -> Result<HttpResponse, HttpError> {
    /// #     Ok(HttpResponse { status_code: 200, body: "{}".into() }) } }
    /// // Missing api_key is rejected.
    /// let result = ClawApi::init(
    ///     ClawApiConfig { backend_type: "openai_compatible".into(), ..Default::default() },
    ///     H,
    /// );
    /// assert!(matches!(result, Err(InitError::MissingApiKey)));
    /// ```
    pub fn init(mut config: ClawApiConfig, http: H) -> Result<ClawApi<H>, InitError> {
        // Centralized credential/config validation. Backends trust that these
        // are present and non-empty once `init` returns Ok.
        if empty(&config.api_key) {
            return Err(InitError::MissingApiKey);
        }
        if empty(&config.model) {
            return Err(InitError::MissingModel);
        }
        if empty(&config.base_url) {
            return Err(InitError::MissingBaseUrl);
        }
        if config.backend_type.is_empty() {
            return Err(InitError::MissingBackendType);
        }

        let registration = find_builtin_registration::<H>(&config.backend_type)
            .ok_or(InitError::UnknownBackend)?;

        // Apply defaults exactly as the C code does.
        if empty(&config.auth_type) {
            config.auth_type = Some(registration.defaults.auth_type.to_string());
        }
        if config.timeout_ms == 0 {
            config.timeout_ms = DEFAULT_TIMEOUT_MS;
        }
        if config.max_tokens == 0 {
            config.max_tokens = DEFAULT_MAX_TOKENS;
        }
        if config.image_max_bytes == 0 {
            config.image_max_bytes = DEFAULT_IMAGE_MAX_BYTES;
        }
        if empty(&config.max_tokens_field) {
            config.max_tokens_field = Some(registration.defaults.max_tokens_field.to_string());
        }

        let supports_json_schema = match config.supports_json_schema {
            Some(enabled) => enabled,
            None => {
                config.backend_type == openai_compatible::ID || config.backend_type == anthropic::ID
            }
        };

        let profile = ModelProfile {
            chat_path: registration.defaults.chat_path.to_string(),
            max_tokens_field: config.max_tokens_field.clone().unwrap_or_default(),
            supports_tools: config.supports_tools,
            supports_vision: config.supports_vision,
            supports_json_schema,
            image_remote_url_only: config.image_remote_url_only,
        };

        let backend = (registration.make)(&config)?;

        Ok(ClawApi {
            profile,
            backend,
            http,
        })
    }

    /// Run a chat completion. (Port of `claw_llm_runtime_chat`.)
    ///
    /// Returns the assistant text and/or any tool calls in an [`LlmResponse`].
    /// Transient transport failures are retried per `request.retry` (defaulting
    /// to [`RetryPolicy::default`](crate::RetryPolicy::default)); set the abort
    /// flag to cancel mid-flight or mid-backoff.
    ///
    /// # Errors
    ///
    /// [`ChatError`]: invalid/unsupported tools, or a wrapped
    /// [`ClawApiError`](crate::ClawApiError) for transport/parse failures. Use
    /// [`ChatError::is_retryable`](crate::ChatError::is_retryable) to inspect.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use std::sync::atomic::AtomicBool;
    /// use claw_api::{ChatRequest, ClawApi, RetryPolicy};
    /// # use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    /// # struct H; impl ClawHttp for H { fn post_json(&mut self, _r: &HttpJsonRequest, _a: &AtomicBool) -> Result<HttpResponse, HttpError> { unimplemented!() } }
    /// # let mut api: ClawApi<H> = unimplemented!();
    /// let messages = serde_json::json!([
    ///     { "role": "user", "content": "What is 2 + 2?" }
    /// ]);
    /// let abort = AtomicBool::new(false);
    /// let resp = api.chat(
    ///     &ChatRequest::new("You are concise.", &messages)
    ///         .with_retry(RetryPolicy::fixed(3, 250)), // 3 retries, 250ms apart
    ///     &abort,
    /// )?;
    /// if let Some(text) = resp.text {
    ///     println!("{text}");
    /// }
    /// # Ok::<(), claw_api::ChatError>(())
    /// ```
    pub fn chat(
        &mut self,
        request: &ChatRequest,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        let policy = request.retry;
        let backend = &self.backend;
        let profile = &self.profile;
        let http = &mut self.http;
        run_with_retry(
            &policy,
            abort,
            ChatError::is_retryable,
            || ChatError::Api(ClawApiError::Transport(ABORTED_DURING_BACKOFF.to_string())),
            || backend.chat(&mut *http, profile, request, abort),
        )
    }

    /// Structured JSON chat: parse the model's reply into `T`.
    ///
    /// `T` only needs [`serde::Deserialize`]. The request **must** carry an
    /// output schema via
    /// [`ChatJsonRequest::with_output_schema`](crate::ChatJsonRequest::with_output_schema).
    /// Backends that advertise JSON-schema support (OpenAI `response_format`,
    /// Anthropic `output_config`) use it natively; others fall back to embedding
    /// the schema in the system prompt.
    ///
    /// `output` is `None` when the model returned only tool calls (and no JSON).
    /// Retry behaves as [`chat`](ClawApi::chat): only transient transport errors
    /// are retried; schema/parse failures are returned immediately.
    ///
    /// # Errors
    ///
    /// [`ChatJsonError`]: [`MissingOutputSchema`](crate::ChatJsonError::MissingOutputSchema)
    /// if no schema was set, [`InvalidOutput`](crate::ChatJsonError::InvalidOutput)
    /// if the reply was not valid JSON for `T`, [`EmptyText`](crate::ChatJsonError::EmptyText)
    /// if there was neither JSON nor a tool call, or a wrapped [`ChatError`] for
    /// transport failures.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use std::sync::atomic::AtomicBool;
    /// use claw_api::{ChatJsonRequest, ClawApi};
    /// # use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    /// # struct H; impl ClawHttp for H { fn post_json(&mut self, _r: &HttpJsonRequest, _a: &AtomicBool) -> Result<HttpResponse, HttpError> { unimplemented!() } }
    /// # let mut api: ClawApi<H> = unimplemented!();
    ///
    /// #[derive(serde::Deserialize)]
    /// struct Sentiment { label: String, score: f32 }
    ///
    /// let schema = r#"{
    ///     "type": "object",
    ///     "properties": {
    ///         "label": { "type": "string" },
    ///         "score": { "type": "number" }
    ///     },
    ///     "required": ["label", "score"]
    /// }"#;
    /// let messages = serde_json::json!([
    ///     { "role": "user", "content": "Classify: 'I love this!'" }
    /// ]);
    /// let abort = AtomicBool::new(false);
    /// let resp = api.chat_json::<Sentiment>(
    ///     &ChatJsonRequest::new("Classify sentiment.", &messages)
    ///         .with_output_schema("sentiment", schema),
    ///     &abort,
    /// )?;
    /// if let Some(s) = resp.output {
    ///     println!("{} ({})", s.label, s.score);
    /// }
    /// # Ok::<(), claw_api::ChatJsonError>(())
    /// ```
    pub fn chat_json<T: DeserializeOwned + Send>(
        &mut self,
        request: &ChatJsonRequest<'_>,
        abort: &AtomicBool,
    ) -> Result<ChatJsonResponse<T>, ChatJsonError> {
        let spec = request
            .output_schema
            .ok_or(ChatJsonError::MissingOutputSchema)?;
        let schema: Value = serde_json::from_str(spec.json)
            .map_err(|err| ChatJsonError::InvalidOutput(format!("invalid schema json: {err}")))?;
        if request.tools_json.filter(|s| !s.is_empty()).is_some() && !self.profile.supports_tools {
            return Err(ChatJsonError::ToolsUnsupported);
        }

        let policy = request.retry;
        let backend = &self.backend;
        let profile = &self.profile;
        let http = &mut self.http;
        run_with_retry(
            &policy,
            abort,
            ChatJsonError::is_retryable,
            || {
                ChatJsonError::Chat(ChatError::Api(ClawApiError::Transport(
                    ABORTED_DURING_BACKOFF.to_string(),
                )))
            },
            || {
                let response = backend
                    .chat_json(&mut *http, profile, request, spec.name, &schema, abort)
                    .map_err(ChatJsonError::from)?;
                Self::parse_chat_json_response(response)
            },
        )
    }

    fn parse_chat_json_response<T: DeserializeOwned + Send>(
        response: LlmResponse,
    ) -> Result<ChatJsonResponse<T>, ChatJsonError> {
        let output = match response.text {
            Some(ref text) if !text.trim().is_empty() => Some(
                serde_json::from_str(text)
                    .map_err(|err| ChatJsonError::InvalidOutput(err.to_string()))?,
            ),
            _ => None,
        };

        if output.is_none() && response.tool_calls.is_empty() {
            return Err(ChatJsonError::EmptyText);
        }

        Ok(ChatJsonResponse {
            output,
            tool_calls: response.tool_calls,
            reasoning_content: response.reasoning_content,
            raw_message_json: response.raw_message_json,
        })
    }

    /// Run one-shot image inference: send image(s) + prompts and return the
    /// model's text. (Port of `claw_llm_runtime_infer_media`.)
    ///
    /// Local image files are read and base64-encoded into a data URL (jpg/jpeg/
    /// png/gif/webp, up to `config.image_max_bytes`); remote URLs are passed
    /// through. Transient transport failures are retried per `request.retry`;
    /// note the media payload is re-prepared (re-read/re-encoded) on each retry.
    ///
    /// # Errors
    ///
    /// [`InferMediaError`]: media-prep failures (missing/oversized/unsupported
    /// file, non-absolute path, ...) or a wrapped
    /// [`ClawApiError`](crate::ClawApiError) for transport failures.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use std::sync::atomic::AtomicBool;
    /// use claw_api::{ClawApi, MediaAsset, MediaRequest};
    /// # use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    /// # struct H; impl ClawHttp for H { fn post_json(&mut self, _r: &HttpJsonRequest, _a: &AtomicBool) -> Result<HttpResponse, HttpError> { unimplemented!() } }
    /// # let mut api: ClawApi<H> = unimplemented!();
    /// let assets = [MediaAsset::local_path("/sdcard/photo.jpg")];
    /// let abort = AtomicBool::new(false);
    /// let description = api.infer_media(
    ///     &MediaRequest::new(&assets).with_user_prompt("Describe this image."),
    ///     &abort,
    /// )?;
    /// println!("{description}");
    /// # Ok::<(), claw_api::InferMediaError>(())
    /// ```
    pub fn infer_media(
        &mut self,
        request: &MediaRequest,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError> {
        let policy = request.retry;
        let backend = &self.backend;
        let profile = &self.profile;
        let http = &mut self.http;
        run_with_retry(
            &policy,
            abort,
            InferMediaError::is_retryable,
            || InferMediaError::Api(ClawApiError::Transport(ABORTED_DURING_BACKOFF.to_string())),
            || backend.infer_media(&mut *http, profile, request, abort),
        )
    }

    /// The resolved [`ModelProfile`](crate::ModelProfile): capability flags
    /// (`supports_tools`, `supports_vision`, `supports_json_schema`) and derived
    /// paths/fields, after defaulting in [`init`](ClawApi::init).
    pub fn profile(&self) -> &ModelProfile {
        &self.profile
    }
}
