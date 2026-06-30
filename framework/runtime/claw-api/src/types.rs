//! Request, response, and configuration types for [`crate::ClawApi`].

/// A tool/function call requested by the model in a chat response.
///
/// Present in [`LlmResponse::tool_calls`] (and [`ChatJsonResponse::tool_calls`]).
/// `arguments_json` is the raw JSON argument object as a string — parse it with
/// `serde_json` against your tool's parameter type.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ToolCall {
    /// Provider-assigned call id, echoed back when you return the tool result.
    pub id: String,
    /// The tool/function name the model wants to invoke.
    pub name: String,
    /// Raw JSON arguments object, as a string (may be empty).
    pub arguments_json: String,
}

/// The result of [`crate::ClawApi::chat`].
///
/// `text` is the assistant message (may be `None` when the model only returned
/// tool calls). `tool_calls` is empty unless the model invoked tools.
#[derive(Clone, Debug, Default)]
pub struct LlmResponse {
    /// Assistant text content, if any.
    pub text: Option<String>,
    /// Provider "thinking"/reasoning text, when the model/provider emits it.
    pub reasoning_content: Option<String>,
    /// The raw assistant message JSON, for callers that need the original shape.
    pub raw_message_json: Option<String>,
    /// Tool calls the model requested, in order.
    pub tool_calls: Vec<ToolCall>,
}

/// Inputs to [`crate::ClawApi::init`].
///
/// Only `api_key`, `backend_type`, and `model` are required; the rest take
/// backend defaults (see [`ClawApi::init`](crate::ClawApi::init)). Build it with
/// struct-update syntax over [`Default`]:
///
/// ```
/// use claw_api::ClawApiConfig;
/// let config = ClawApiConfig {
///     api_key: Some("sk-...".into()),
///     backend_type: "openai_compatible".into(),
///     model: Some("gpt-4o-mini".into()),
///     base_url: Some("https://api.openai.com/v1".into()),
///     supports_tools: true,
///     supports_vision: true,
///     ..Default::default()
/// };
/// # let _ = config;
/// ```
#[derive(Clone, Debug, Default)]
pub struct ClawApiConfig {
    /// Provider API key (required).
    pub api_key: Option<String>,
    /// Backend id: `"openai_compatible"` or `"anthropic_compatible"` (required).
    pub backend_type: String,
    /// Model name sent to the provider (required).
    pub model: Option<String>,
    /// API base URL, e.g. `"https://api.openai.com/v1"`.
    pub base_url: Option<String>,
    /// Auth scheme override (`"bearer"`, `"api-key"`, `"none"`); defaults per backend.
    pub auth_type: Option<String>,
    /// Override the max-tokens field name; defaults per backend.
    pub max_tokens_field: Option<String>,
    /// Per-request HTTP timeout; `0` applies the 120s default.
    pub timeout_ms: u32,
    /// Max output tokens; `0` applies the default (8192).
    pub max_tokens: u32,
    /// Max local image size for [`crate::ClawApi::infer_media`]; `0` applies 512KiB.
    pub image_max_bytes: usize,
    /// Advertise tool-call support to the client.
    pub supports_tools: bool,
    /// Advertise vision/media support.
    pub supports_vision: bool,
    /// API-level JSON schema support. `None` enables it for backends that
    /// support it by default (`openai_compatible`, `anthropic_compatible`).
    pub supports_json_schema: Option<bool>,
    /// When set, only remote image URLs are accepted (no local data URLs).
    pub image_remote_url_only: bool,
}

/// Default retry interval (backoff before the first retry), in milliseconds.
pub const DEFAULT_RETRY_INTERVAL_MS: u32 = 500;
/// Default number of extra attempts after the first try.
pub const DEFAULT_MAX_RETRIES: u32 = 2;
/// Default upper bound on any single backoff, in milliseconds.
pub const DEFAULT_MAX_BACKOFF_MS: u32 = 8_000;
/// Default backoff growth factor (`2` = exponential).
pub const DEFAULT_BACKOFF_MULTIPLIER: u32 = 2;

/// Per-call retry policy, set via `with_retry` on a request
/// ([`ChatRequest::with_retry`], [`ChatJsonRequest::with_retry`],
/// [`MediaRequest::with_retry`]).
///
/// Only transient failures are retried (network errors, HTTP 408/429/5xx).
/// Aborts and deterministic client errors (bad URL/body, 4xx) are never retried.
/// Backoff before retry _n_ is `initial_backoff_ms * backoff_multiplier^(n-1)`,
/// capped at `max_backoff_ms`.
///
/// # Examples
///
/// ```
/// use claw_api::RetryPolicy;
///
/// // Default: 2 retries, 500ms interval, exponential, capped at 8s.
/// let p = RetryPolicy::default();
/// assert_eq!(p.backoff_ms(1), 500);
/// assert_eq!(p.backoff_ms(2), 1000);
///
/// // 3 retries at a fixed 250ms interval.
/// let fixed = RetryPolicy::fixed(3, 250);
/// assert_eq!(fixed.backoff_ms(1), 250);
/// assert_eq!(fixed.backoff_ms(2), 250);
///
/// // Custom interval, default count, via builder.
/// let custom = RetryPolicy::new(2).with_interval_ms(1_000);
/// assert_eq!(custom.backoff_ms(1), 1_000);
///
/// // Disable retry entirely.
/// assert_eq!(RetryPolicy::none().max_retries, 0);
/// ```
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RetryPolicy {
    /// Extra attempts after the first try (`0` disables retry).
    pub max_retries: u32,
    /// Retry interval: backoff before the first retry, in milliseconds.
    pub initial_backoff_ms: u32,
    /// Upper bound applied to any single backoff, in milliseconds.
    pub max_backoff_ms: u32,
    /// Backoff growth factor applied after each retry (`2` = exponential).
    pub backoff_multiplier: u32,
}

impl Default for RetryPolicy {
    fn default() -> Self {
        RetryPolicy::new(DEFAULT_MAX_RETRIES)
    }
}

impl RetryPolicy {
    /// Retry `max_retries` times with the default 500ms interval (exponential,
    /// capped at 8s). Tweak the interval with [`RetryPolicy::with_interval_ms`].
    pub const fn new(max_retries: u32) -> Self {
        RetryPolicy {
            max_retries,
            initial_backoff_ms: DEFAULT_RETRY_INTERVAL_MS,
            max_backoff_ms: DEFAULT_MAX_BACKOFF_MS,
            backoff_multiplier: DEFAULT_BACKOFF_MULTIPLIER,
        }
    }

    /// Retry `max_retries` times at a fixed interval (no exponential growth).
    pub const fn fixed(max_retries: u32, interval_ms: u32) -> Self {
        RetryPolicy {
            max_retries,
            initial_backoff_ms: interval_ms,
            max_backoff_ms: interval_ms,
            backoff_multiplier: 1,
        }
    }

    /// Override the retry interval (backoff before the first retry).
    pub const fn with_interval_ms(mut self, interval_ms: u32) -> Self {
        self.initial_backoff_ms = interval_ms;
        self
    }

    /// Override the cap applied to any single backoff.
    pub const fn with_max_backoff_ms(mut self, max_backoff_ms: u32) -> Self {
        self.max_backoff_ms = max_backoff_ms;
        self
    }

    /// Override the backoff growth factor (`1` = fixed interval).
    pub const fn with_multiplier(mut self, backoff_multiplier: u32) -> Self {
        self.backoff_multiplier = backoff_multiplier;
        self
    }

    /// A policy that never retries.
    pub const fn none() -> Self {
        RetryPolicy {
            max_retries: 0,
            initial_backoff_ms: 0,
            max_backoff_ms: 0,
            backoff_multiplier: 2,
        }
    }

    /// Capped backoff (ms) before the given 1-based retry `attempt`.
    pub fn backoff_ms(&self, attempt: u32) -> u32 {
        if attempt == 0 {
            return 0;
        }
        let multiplier = self.backoff_multiplier.max(1);
        let mut backoff = self.initial_backoff_ms;
        for _ in 1..attempt {
            backoff = backoff.saturating_mul(multiplier);
            if backoff >= self.max_backoff_ms {
                return self.max_backoff_ms;
            }
        }
        backoff.min(self.max_backoff_ms)
    }
}

/// The resolved model capabilities and derived endpoint settings, as computed
/// by [`crate::ClawApi::init`]. Read it back via [`crate::ClawApi::profile`].
#[derive(Clone, Debug, Default)]
pub struct ModelProfile {
    /// Chat endpoint path appended to the base URL (e.g. `"/chat/completions"`).
    ///
    /// Backend plumbing detail; not part of the public surface.
    pub(crate) chat_path: String,
    /// The provider field name carrying the max-tokens value.
    ///
    /// Backend plumbing detail; not part of the public surface.
    pub(crate) max_tokens_field: String,
    /// Whether tool calls may be sent.
    pub supports_tools: bool,
    /// Whether image/media inference is available.
    pub supports_vision: bool,
    /// Whether API-level JSON schema is used (vs. schema-in-prompt fallback).
    pub supports_json_schema: bool,
    /// Whether only remote image URLs are accepted.
    pub image_remote_url_only: bool,
}

/// A named JSON Schema for structured output, attached to a
/// [`ChatJsonRequest`] via [`ChatJsonRequest::with_output_schema`].
#[derive(Clone, Copy, Debug)]
pub struct StaticOutputSchema<'a> {
    /// Schema name reported to the provider (e.g. `"sentiment"`).
    pub name: &'a str,
    /// The JSON Schema document, as a JSON string.
    pub json: &'a str,
}

/// A request for [`crate::ClawApi::chat_json`] (structured JSON output).
///
/// `messages` is a JSON array of chat messages (e.g.
/// `[{ "role": "user", "content": "..." }]`). An output schema is **required**
/// — set it with [`with_output_schema`](ChatJsonRequest::with_output_schema).
/// Tools are optional; the per-call [`RetryPolicy`] defaults and is overridable
/// via [`with_retry`](ChatJsonRequest::with_retry).
///
/// ```
/// use claw_api::ChatJsonRequest;
/// let messages = serde_json::json!([{ "role": "user", "content": "hi" }]);
/// let schema = r#"{"type":"object","properties":{"ok":{"type":"boolean"}}}"#;
/// let req = ChatJsonRequest::new("be terse", &messages)
///     .with_output_schema("answer", schema);
/// # let _ = req;
/// ```
pub struct ChatJsonRequest<'a> {
    /// System prompt / instructions.
    pub system_prompt: &'a str,
    /// JSON array of chat messages (the persisted history segment).
    pub messages: &'a serde_json::Value,
    /// Ephemeral trailing messages appended after `messages` for this request
    /// only (never persisted). Kept as a separate segment so the history is not
    /// cloned to append them; the backend iterates `messages` then `reminders`.
    /// Defaults to empty; set with [`with_reminders`](Self::with_reminders).
    pub reminders: &'a [serde_json::Value],
    /// Optional OpenAI-style tools JSON array.
    pub tools_json: Option<&'a str>,
    /// The required output schema (set via [`Self::with_output_schema`]).
    pub output_schema: Option<StaticOutputSchema<'a>>,
    /// Per-call retry policy. Defaults to [`RetryPolicy::default`]; use
    /// [`RetryPolicy::none`] to disable retry.
    pub retry: RetryPolicy,
}

impl<'a> ChatJsonRequest<'a> {
    /// A structured-output request (no schema/tools yet).
    pub fn new(system_prompt: &'a str, messages: &'a serde_json::Value) -> Self {
        Self {
            system_prompt,
            messages,
            reminders: &[],
            tools_json: None,
            output_schema: None,
            retry: RetryPolicy::default(),
        }
    }

    /// Attach an OpenAI-style tools JSON array (may be sent with `response_format`).
    pub fn with_tools(mut self, tools_json: &'a str) -> Self {
        self.tools_json = Some(tools_json);
        self
    }

    /// Attach ephemeral trailing reminder messages for this request only.
    pub fn with_reminders(mut self, reminders: &'a [serde_json::Value]) -> Self {
        self.reminders = reminders;
        self
    }

    /// Attach a static JSON Schema (`name` + schema JSON string).
    pub fn with_output_schema(mut self, name: &'a str, schema_json: &'a str) -> Self {
        self.output_schema = Some(StaticOutputSchema {
            name,
            json: schema_json,
        });
        self
    }

    /// Override the retry policy for this call.
    pub fn with_retry(mut self, retry: RetryPolicy) -> Self {
        self.retry = retry;
        self
    }
}

/// The result of [`crate::ClawApi::chat_json`].
///
/// `output` is the reply parsed into `T`, or `None` when the model returned only
/// tool calls. `T` is whatever you asked [`chat_json`](crate::ClawApi::chat_json)
/// to deserialize.
#[derive(Clone, Debug)]
pub struct ChatJsonResponse<T> {
    /// The parsed structured output, if the model produced JSON.
    pub output: Option<T>,
    /// Tool calls the model requested, in order.
    pub tool_calls: Vec<ToolCall>,
    /// Provider reasoning/"thinking" text, when emitted.
    pub reasoning_content: Option<String>,
    /// The raw assistant message JSON.
    pub raw_message_json: Option<String>,
}

/// A request for [`crate::ClawApi::chat`].
///
/// `messages` is a JSON array of chat messages (e.g.
/// `[{ "role": "user", "content": "..." }]`). Tools are optional; the per-call
/// [`RetryPolicy`] defaults and is overridable via
/// [`with_retry`](ChatRequest::with_retry).
///
/// ```
/// use claw_api::{ChatRequest, RetryPolicy};
/// let messages = serde_json::json!([{ "role": "user", "content": "hi" }]);
/// let req = ChatRequest::new("be terse", &messages)
///     .with_retry(RetryPolicy::fixed(3, 250));
/// # let _ = req;
/// ```
pub struct ChatRequest<'a> {
    /// System prompt / instructions.
    pub system_prompt: &'a str,
    /// JSON array of chat messages (the persisted history segment).
    pub messages: &'a serde_json::Value,
    /// Ephemeral trailing messages appended after `messages` for this request
    /// only (never persisted). Kept as a separate segment so the history is not
    /// cloned to append them; the backend iterates `messages` then `reminders`.
    /// Defaults to empty; set with [`with_reminders`](Self::with_reminders).
    pub reminders: &'a [serde_json::Value],
    /// Optional OpenAI-style tools JSON array.
    pub tools_json: Option<&'a str>,
    /// Per-call retry policy. Defaults to [`RetryPolicy::default`]; use
    /// [`RetryPolicy::none`] to disable retry.
    pub retry: RetryPolicy,
}

impl<'a> ChatRequest<'a> {
    /// A tool-less chat request.
    pub fn new(system_prompt: &'a str, messages: &'a serde_json::Value) -> Self {
        ChatRequest {
            system_prompt,
            messages,
            reminders: &[],
            tools_json: None,
            retry: RetryPolicy::default(),
        }
    }

    /// Attach an OpenAI-style tools JSON array.
    pub fn with_tools(mut self, tools_json: &'a str) -> Self {
        self.tools_json = Some(tools_json);
        self
    }

    /// Attach ephemeral trailing reminder messages for this request only.
    pub fn with_reminders(mut self, reminders: &'a [serde_json::Value]) -> Self {
        self.reminders = reminders;
        self
    }

    /// Override the retry policy for this call.
    pub fn with_retry(mut self, retry: RetryPolicy) -> Self {
        self.retry = retry;
        self
    }
}

/// How a [`MediaAsset`] supplies its image data.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AssetKind {
    /// An absolute local file path (read + base64-encoded into a data URL).
    LocalPath,
    /// A remote image URL (passed through to the provider).
    RemoteUrl,
    /// Inline bytes (base64-encoded into a data URL; requires explicit MIME type).
    InlineBytes,
}

/// An image input for [`crate::ClawApi::infer_media`].
///
/// Construct with [`MediaAsset::local_path`], [`MediaAsset::remote_url`], or
/// [`MediaAsset::inline_bytes`]. Supported local types: jpg/jpeg/png/gif/webp.
///
/// ```
/// use claw_api::MediaAsset;
/// let a = MediaAsset::local_path("/sdcard/photo.jpg");
/// let b = MediaAsset::remote_url("https://example.com/cat.png");
/// # let _ = (a, b);
/// ```
#[derive(Clone, Debug)]
pub struct MediaAsset {
    /// Which of the fields below is populated.
    pub kind: AssetKind,
    /// Absolute file path, for [`AssetKind::LocalPath`].
    pub path: Option<String>,
    /// Image URL, for [`AssetKind::RemoteUrl`].
    pub url: Option<String>,
    /// Raw bytes, for [`AssetKind::InlineBytes`].
    pub bytes: Option<Vec<u8>>,
    /// MIME type override; otherwise inferred from the file extension.
    pub mime_type: Option<String>,
}

impl MediaAsset {
    /// An asset backed by a local file path.
    pub fn local_path(path: impl Into<String>) -> Self {
        MediaAsset {
            kind: AssetKind::LocalPath,
            path: Some(path.into()),
            url: None,
            bytes: None,
            mime_type: None,
        }
    }

    /// An asset referenced by a remote URL.
    pub fn remote_url(url: impl Into<String>) -> Self {
        MediaAsset {
            kind: AssetKind::RemoteUrl,
            path: None,
            url: Some(url.into()),
            bytes: None,
            mime_type: None,
        }
    }

    /// An asset carrying inline bytes with an explicit MIME type.
    pub fn inline_bytes(bytes: Vec<u8>, mime_type: impl Into<String>) -> Self {
        MediaAsset {
            kind: AssetKind::InlineBytes,
            path: None,
            url: None,
            bytes: Some(bytes),
            mime_type: Some(mime_type.into()),
        }
    }

    /// Override the MIME type (otherwise inferred from the file extension).
    pub fn with_mime_type(mut self, mime_type: impl Into<String>) -> Self {
        self.mime_type = Some(mime_type.into());
        self
    }
}

/// A request for [`crate::ClawApi::infer_media`]: image(s) plus optional prompts.
///
/// ```
/// use claw_api::{MediaAsset, MediaRequest};
/// let assets = [MediaAsset::local_path("/sdcard/photo.jpg")];
/// let req = MediaRequest::new(&assets).with_user_prompt("Describe this image.");
/// # let _ = req;
/// ```
pub struct MediaRequest<'a> {
    /// Optional system prompt / instructions.
    pub system_prompt: Option<&'a str>,
    /// Optional user prompt accompanying the image(s).
    pub user_prompt: Option<&'a str>,
    /// The image asset(s) to send.
    pub media: &'a [MediaAsset],
    /// Per-call retry policy. Defaults to [`RetryPolicy::default`]; use
    /// [`RetryPolicy::none`] to disable retry.
    pub retry: RetryPolicy,
}

impl<'a> MediaRequest<'a> {
    /// A media request over the given assets, with no prompts set yet.
    pub fn new(media: &'a [MediaAsset]) -> Self {
        MediaRequest {
            system_prompt: None,
            user_prompt: None,
            media,
            retry: RetryPolicy::default(),
        }
    }

    /// Set the system prompt / instructions.
    pub fn with_system_prompt(mut self, system_prompt: &'a str) -> Self {
        self.system_prompt = Some(system_prompt);
        self
    }

    /// Set the user prompt accompanying the image(s).
    pub fn with_user_prompt(mut self, user_prompt: &'a str) -> Self {
        self.user_prompt = Some(user_prompt);
        self
    }

    /// Override the retry policy for this call.
    pub fn with_retry(mut self, retry: RetryPolicy) -> Self {
        self.retry = retry;
        self
    }
}

/// Internal: how a prepared media payload is encoded (`claw_media_prepared_kind_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PreparedKind {
    DataUrl,
    RemoteUrl,
}

/// Internal: output of the media-prep pipeline (`claw_media_prepared_t`).
#[derive(Clone, Debug)]
pub(crate) struct Prepared {
    pub kind: PreparedKind,
    /// Data URL (for [`PreparedKind::DataUrl`]) or the remote URL.
    pub payload: String,
}
