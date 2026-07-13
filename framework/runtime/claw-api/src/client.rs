//! `ClawApi` — the LLM client, port of `claw_llm_runtime.c`.
//!
//! Owns an injected HTTP transport and an optional resolved backend. Construct a
//! client with [`ClawApi::new`], then install a complete config with
//! [`ClawApi::set_config`] before issuing requests.

use core::sync::atomic::AtomicBool;

use serde::de::DeserializeOwned;
use serde_json::Value;
use tracing::Instrument as _;

use claw_interface::http::blocking::ClawHttp as BlockingClawHttp;
use claw_interface::http::StreamingHttp;
use claw_interface::{Cancel, ClawHttp, ClawTimer};

use super::backends::Backend;
use super::errors::{ChatError, ChatJsonError, ClawApiError, InferMediaError, InitError};
use super::retry::{run_with_retry, sleep_abortable_async};
use super::stream::ChatStream;
use super::types::{
    ChatJsonRequest, ChatJsonResponse, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest,
};

/// Message used when the abort flag fires during a retry backoff sleep. Kept
/// containing "aborted" so upstream string-based abort detection still matches.
const ABORTED_DURING_BACKOFF: &str = "LLM request aborted during retry backoff";

/// The LLM client: a resolved backend + model profile behind an injected
/// [`ClawHttp`] transport.
///
/// Construct it once with [`ClawApi::new`], configure it with
/// [`ClawApi::set_config`], then reuse it for many requests.
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
/// use claw_api::{BackendKind, ChatRequest, ClawApi, ClawApiConfig};
/// # use claw_interface::http::blocking::ClawHttp;
/// # use claw_interface::http::{HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode};
/// # struct H; impl ClawHttp for H {
/// #   fn post_json(&mut self, _r: &HttpJsonRequest, _a: &AtomicBool) -> Result<HttpResponse, HttpError> {
/// #     Ok(HttpResponse { status_code: HttpStatusCode::OK, body: r#"{"choices":[{"message":{"role":"assistant","content":"ok"}}]}"#.into() }) } }
/// let mut api = ClawApi::new(H);
/// api.set_config(ClawApiConfig::new(
///         BackendKind::OpenAiCompatible,
///         "sk-...",
///         "gpt-4o-mini",
///         "https://api.openai.com/v1",
///     ))?;
/// let msgs = serde_json::json!([{ "role": "user", "content": "hi" }]);
/// let abort = AtomicBool::new(false);
/// let resp = api.chat(&ChatRequest::new("sys", &msgs), &abort)?;
/// # Ok::<(), anyhow::Error>(())
/// ```
pub struct ClawApi<H: BlockingClawHttp> {
    backend: Option<Backend>,
    http: H,
}

/// Async LLM client: a resolved backend behind an injected [`ClawHttp`]
/// transport and [`ClawTimer`] backoff timer.
pub struct ClawApiAsync<H: ClawHttp, Timer: ClawTimer> {
    backend: Option<Backend>,
    http: H,
    timer: Timer,
}

fn resolve_config(config: ClawApiConfig) -> Result<Backend, InitError> {
    // Backends trust that required fields are present once `set_config` returns.
    config.validate()?;
    config.backend.make(&config)
}

fn parse_chat_json_response<T: DeserializeOwned>(
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

/// Stable, shape-only trace classification; never expose an error's payload.
fn chat_error_kind(error: &ChatError) -> &'static str {
    match error {
        ChatError::Api(error) => error.into(),
        other => other.into(),
    }
}

impl<H: BlockingClawHttp> ClawApi<H> {
    /// Construct an unconfigured client over `http`.
    #[must_use]
    pub fn new(http: H) -> Self {
        Self {
            backend: None,
            http,
        }
    }

    /// Validate and atomically install a complete backend config.
    ///
    /// # Errors
    ///
    /// Returns [`InitError`] when a required config field is empty.
    pub fn set_config(&mut self, config: ClawApiConfig) -> Result<(), InitError> {
        self.backend = Some(resolve_config(config)?);
        Ok(())
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
    /// # use claw_interface::http::blocking::ClawHttp;
    /// # use claw_interface::http::{HttpError, HttpJsonRequest, HttpResponse};
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
        let backend = self.backend.as_ref().ok_or(ClawApiError::NotConfigured)?;
        let http = &mut self.http;
        run_with_retry(
            &policy,
            abort,
            ChatError::is_retryable,
            || ChatError::Api(ClawApiError::Transport(ABORTED_DURING_BACKOFF.to_string())),
            || backend.chat(&mut *http, request, abort),
        )
    }

    /// Structured JSON chat: parse the model's reply into `T`.
    ///
    /// `T` only needs [`serde::Deserialize`]. The request **must** carry an
    /// output schema via
    /// [`ChatJsonRequest::with_output_schema`](crate::ChatJsonRequest::with_output_schema).
    /// The backend sends the schema natively (OpenAI `response_format`,
    /// Anthropic `output_config`).
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
    /// # use claw_interface::http::blocking::ClawHttp;
    /// # use claw_interface::http::{HttpError, HttpJsonRequest, HttpResponse};
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
    pub fn chat_json<T: DeserializeOwned>(
        &mut self,
        request: &ChatJsonRequest<'_>,
        abort: &AtomicBool,
    ) -> Result<ChatJsonResponse<T>, ChatJsonError> {
        let spec = request
            .output_schema
            .ok_or(ChatJsonError::MissingOutputSchema)?;
        let schema: Value = serde_json::from_str(spec.json)
            .map_err(|err| ChatJsonError::InvalidOutput(format!("invalid schema json: {err}")))?;

        let policy = request.retry;
        let backend = self
            .backend
            .as_ref()
            .ok_or(ClawApiError::NotConfigured)
            .map_err(ChatError::from)?;
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
                    .chat_json(&mut *http, request, spec.name, &schema, abort)
                    .map_err(ChatJsonError::from)?;
                parse_chat_json_response(response)
            },
        )
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
    /// # use claw_interface::http::blocking::ClawHttp;
    /// # use claw_interface::http::{HttpError, HttpJsonRequest, HttpResponse};
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
        let backend = self.backend.as_ref().ok_or(ClawApiError::NotConfigured)?;
        let http = &mut self.http;
        run_with_retry(
            &policy,
            abort,
            InferMediaError::is_retryable,
            || InferMediaError::Api(ClawApiError::Transport(ABORTED_DURING_BACKOFF.to_string())),
            || backend.infer_media(&mut *http, request, abort),
        )
    }
}

impl<H: ClawHttp, Timer: ClawTimer> ClawApiAsync<H, Timer> {
    /// Construct an unconfigured client over the supplied transport and timer.
    #[must_use]
    pub fn new(http: H, timer: Timer) -> Self {
        Self {
            backend: None,
            http,
            timer,
        }
    }

    /// Rebind this client to a new [`ClawApiConfig`] at runtime, keeping the
    /// existing HTTP transport and timer (so a keep-alive connection is not torn
    /// down). Only the backend — provider, key, model, base URL — is rebuilt.
    ///
    /// Used to apply a per-turn config selected from a `ClawApiManager` without
    /// reconstructing the whole client. Returns [`InitError`] if the new config
    /// is incomplete or invalid.
    pub fn set_config(&mut self, config: ClawApiConfig) -> Result<(), InitError> {
        self.backend = Some(resolve_config(config)?);
        Ok(())
    }

    /// Async chat completion over [`ClawHttp`].
    pub async fn chat(
        &mut self,
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        let backend = self
            .backend
            .as_ref()
            .ok_or(ClawApiError::NotConfigured)
            .map_err(ChatError::from)?;
        let policy = request.retry;
        let max_attempts = u64::from(policy.max_retries).saturating_add(1);
        let mut retry_attempt = 0u32;
        loop {
            let attempt = u64::from(retry_attempt).saturating_add(1);
            let result = async {
                let result = backend.chat_async(&mut self.http, request, cancel).await;
                match &result {
                    Ok(_) => tracing::info!(name: "completed", ""),
                    Err(error) => {
                        let kind = chat_error_kind(error);
                        let retryable = error.is_retryable();
                        let final_attempt = !retryable || retry_attempt >= policy.max_retries;
                        if final_attempt {
                            tracing::error!(
                                name: "failed",
                                kind,
                                retryable,
                                final = true
                            );
                        } else {
                            tracing::warn!(
                                name: "failed",
                                kind,
                                retryable,
                                final = false
                            );
                        }
                    }
                }
                result
            }
            .instrument(tracing::info_span!("api.attempt", attempt, max_attempts))
            .await;

            match result {
                Ok(response) => return Ok(response),
                Err(error) => {
                    if !error.is_retryable() || retry_attempt >= policy.max_retries {
                        return Err(error);
                    }
                    let error_kind = chat_error_kind(&error);
                    let failed_attempt = attempt;
                    retry_attempt = retry_attempt.saturating_add(1);
                    let next_attempt = u64::from(retry_attempt).saturating_add(1);
                    let backoff_ms = policy.backoff_ms(retry_attempt);
                    let completed = async {
                        let completed =
                            sleep_abortable_async(backoff_ms, &mut self.timer, cancel).await;
                        if completed {
                            tracing::info!(name: "completed", "");
                        } else {
                            tracing::warn!(name: "cancelled", "");
                        }
                        completed
                    }
                    .instrument(tracing::info_span!(
                        "api.retry",
                        failed_attempt,
                        next_attempt,
                        backoff_ms,
                        error_kind
                    ))
                    .await;
                    if !completed {
                        return Err(ChatError::Api(ClawApiError::Transport(
                            ABORTED_DURING_BACKOFF.to_string(),
                        )));
                    }
                }
            }
        }
    }

    /// Streaming chat completion over [`StreamingHttp`].
    ///
    /// Yields [`LlmDelta`](crate::LlmDelta) fragments as tokens arrive — reasoning,
    /// then output, then complete tool calls. Drain it to the end, then call
    /// [`ChatStream::take_response`] for the assembled [`LlmResponse`]. Unlike
    /// [`chat`](Self::chat) it does not retry: a stream cannot be resumed
    /// mid-body, so a failure is surfaced to the caller.
    pub async fn chat_stream<'a>(
        &'a mut self,
        request: &'a ChatRequest<'_>,
        cancel: Cancel<'a>,
    ) -> Result<ChatStream<H::ByteStream>, ChatError>
    where
        H: StreamingHttp,
    {
        self.backend
            .as_ref()
            .ok_or(ClawApiError::NotConfigured)?
            .chat_stream_async(&mut self.http, request, cancel)
            .await
    }

    /// Async structured JSON chat over [`ClawHttp`].
    pub async fn chat_json<T: DeserializeOwned>(
        &mut self,
        request: &ChatJsonRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<ChatJsonResponse<T>, ChatJsonError> {
        let backend = self
            .backend
            .as_ref()
            .ok_or(ClawApiError::NotConfigured)
            .map_err(ChatError::from)?;
        let spec = request
            .output_schema
            .ok_or(ChatJsonError::MissingOutputSchema)?;
        let schema: Value = serde_json::from_str(spec.json)
            .map_err(|err| ChatJsonError::InvalidOutput(format!("invalid schema json: {err}")))?;

        let policy = request.retry;
        let mut attempt = 0u32;
        loop {
            let result = match backend
                .chat_json_async(&mut self.http, request, spec.name, &schema, cancel)
                .await
            {
                Ok(response) => parse_chat_json_response(response),
                Err(error) => Err(ChatJsonError::from(error)),
            };

            match result {
                Ok(response) => return Ok(response),
                Err(error) => {
                    if !error.is_retryable() || attempt >= policy.max_retries {
                        return Err(error);
                    }
                    attempt = attempt.saturating_add(1);
                    if !sleep_abortable_async(policy.backoff_ms(attempt), &mut self.timer, cancel)
                        .await
                    {
                        return Err(ChatJsonError::Chat(ChatError::Api(
                            ClawApiError::Transport(ABORTED_DURING_BACKOFF.to_string()),
                        )));
                    }
                }
            }
        }
    }

    /// Async one-shot image inference over [`ClawHttp`].
    pub async fn infer_media(
        &mut self,
        request: &MediaRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<String, InferMediaError> {
        let backend = self.backend.as_ref().ok_or(ClawApiError::NotConfigured)?;
        let policy = request.retry;
        let mut attempt = 0u32;
        loop {
            match backend
                .infer_media_async(&mut self.http, request, cancel)
                .await
            {
                Ok(response) => return Ok(response),
                Err(error) => {
                    if !error.is_retryable() || attempt >= policy.max_retries {
                        return Err(error);
                    }
                    attempt = attempt.saturating_add(1);
                    if !sleep_abortable_async(policy.backoff_ms(attempt), &mut self.timer, cancel)
                        .await
                    {
                        return Err(InferMediaError::Api(ClawApiError::Transport(
                            ABORTED_DURING_BACKOFF.to_string(),
                        )));
                    }
                }
            }
        }
    }
}
