//! `claw-api` error types.
//!
//! Each public entry point ([`crate::ClawApi::init`], [`crate::ClawApi::chat`],
//! [`crate::ClawApi::infer_media`]) returns its own error enum because their
//! failure modes are not 1-to-1: config validation, chat-only tool errors, and
//! the media pipeline are disjoint. The shared API/transport/parse failures live
//! in [`ClawApiError`], which the per-function enums wrap via `#[from]`.
//!
//! All variants carry only `&'static str` (or, for the genuinely dynamic HTTP
//! transport message, an owned `String`); `Display` text comes from `thiserror`.

use strum::IntoStaticStr;
use thiserror::Error;

/// Failures shared by chat and media calls (transport, response parsing,
/// allocation). `ApiError` is the static-message catch-all.
#[derive(Debug, Clone, IntoStaticStr, PartialEq, Eq, Error)]
pub enum ClawApiError {
    /// Permanent transport failure (aborts, bad URL/body, 4xx, ...). Carries the
    /// backend/transport detail (e.g. `"HTTP 401: invalid api key"`), which is
    /// inherently dynamic. Never retried.
    #[strum(serialize = "transport")]
    #[error("HTTP transport error: {0}")]
    Transport(String),
    /// Transient transport failure (network error, HTTP 408/429/5xx) eligible
    /// for retry by the [`crate::ClawApi`] retry loop.
    #[strum(serialize = "transient_transport")]
    #[error("transient HTTP transport error: {0}")]
    TransientTransport(String),
    /// The response body was not valid JSON.
    #[strum(serialize = "parse")]
    #[error("failed to parse LLM JSON response")]
    Parse,
    /// The model returned no usable content.
    #[strum(serialize = "empty_response")]
    #[error("LLM returned an empty response")]
    EmptyResponse,
    /// The response JSON had an unexpected shape (missing/!assistant message,
    /// missing content, malformed tool call).
    #[strum(serialize = "malformed_response")]
    #[error("malformed LLM response: {0}")]
    MalformedResponse(&'static str),
    /// Any other API-side failure (allocation, serialization, ...).
    #[strum(serialize = "api")]
    #[error("{0}")]
    ApiError(&'static str),
}

impl ClawApiError {
    /// Whether retrying the same request might succeed.
    #[must_use]
    pub fn is_retryable(&self) -> bool {
        matches!(self, ClawApiError::TransientTransport(_))
    }

    /// Whether this failure came from aborting an in-flight request.
    #[must_use]
    pub fn is_aborted(&self) -> bool {
        matches!(self, ClawApiError::Transport(message) if message.contains("aborted"))
    }
}

/// Failures from constructing a [`crate::ClawApi`] (config validation + backend
/// selection).
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum InitError {
    #[error("LLM API key is empty")]
    MissingApiKey,
    #[error("LLM model is empty")]
    MissingModel,
    #[error("LLM base URL is empty")]
    MissingBaseUrl,
}

/// Failures from a structured JSON chat completion request.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum ChatJsonError {
    /// Model returned neither parseable JSON nor tool calls.
    #[error("LLM returned empty structured output")]
    EmptyText,
    /// Parsed text was not valid JSON for the expected output type.
    #[error("invalid structured output: {0}")]
    InvalidOutput(String),
    /// [`crate::ClawApi::chat_json`] was called without
    /// [`crate::ChatJsonRequest::with_output_schema`].
    #[error("structured chat requires an output schema")]
    MissingOutputSchema,
    /// A shared chat completion failure.
    #[error(transparent)]
    Chat(#[from] ChatError),
}

impl ChatJsonError {
    /// Retryable only when the underlying chat transport failure is transient;
    /// schema/parse failures are deterministic and never retried.
    #[must_use]
    pub fn is_retryable(&self) -> bool {
        matches!(self, ChatJsonError::Chat(err) if err.is_retryable())
    }
}

/// Failures from [`crate::ClawApi::chat`].
///
/// Transient transport failures are retried automatically per the request's
/// [`RetryPolicy`](crate::RetryPolicy); a `ChatError` therefore represents a
/// final failure. Use [`ChatError::is_retryable`] to decide whether retrying
/// the whole operation (e.g. after rebuilding the request) is worthwhile.
///
/// ```
/// use claw_api::{ChatError, ClawApiError};
/// fn handle(err: &ChatError) {
///     match err {
///         ChatError::Api(ClawApiError::TransientTransport(msg)) => {
///             eprintln!("transient, may retry: {msg}");
///         }
///         other => eprintln!("permanent failure: {other}"),
///     }
/// }
/// ```
#[derive(Debug, Clone, IntoStaticStr, PartialEq, Eq, Error)]
pub enum ChatError {
    /// The caller-supplied tools JSON was invalid.
    #[strum(serialize = "invalid_tools_json")]
    #[error("invalid tools JSON")]
    InvalidToolsJson,
    /// A shared API/transport/parse failure.
    #[strum(serialize = "api")]
    #[error(transparent)]
    Api(#[from] ClawApiError),
}

impl ChatError {
    /// Whether retrying the same request might succeed.
    #[must_use]
    pub fn is_retryable(&self) -> bool {
        matches!(self, ChatError::Api(err) if err.is_retryable())
    }

    /// Whether this chat request was aborted by the caller.
    #[must_use]
    pub fn is_aborted(&self) -> bool {
        matches!(self, ChatError::Api(err) if err.is_aborted())
    }
}

/// Failures from a one-shot media inference request (includes the media-prep
/// pipeline used only by this call).
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum InferMediaError {
    /// The request was missing a prompt or media asset.
    #[error("media request is incomplete")]
    IncompleteRequest,
    /// Multiple media assets were supplied, but current backends accept one.
    #[error("multiple media assets are not supported")]
    MultipleMediaAssetsUnsupported,
    /// Media path was empty.
    #[error("media path is empty")]
    MediaPathEmpty,
    /// Media path was not absolute.
    #[error("media path must be an absolute path")]
    MediaPathNotAbsolute,
    /// Media URL was empty.
    #[error("media URL is empty")]
    MediaUrlEmpty,
    /// The media file extension/MIME is not a supported image type.
    #[error("only local jpg/jpeg/png/gif/webp files are supported")]
    UnsupportedMediaType,
    /// The media file does not exist.
    #[error("media file not found")]
    MediaNotFound,
    /// The media file was empty.
    #[error("media file is empty")]
    MediaFileEmpty,
    /// The media file exceeded the configured size limit.
    #[error("media file is too large")]
    MediaTooLarge,
    /// Reading the media file failed.
    #[error("failed to read media file")]
    MediaReadFailed,
    /// The asset kind is not supported (e.g. inline bytes).
    #[error("unsupported media asset kind")]
    UnsupportedMediaKind,
    /// The profile only accepts remote image URLs.
    #[error("selected profile only supports remote image URLs")]
    RemoteOnlyProfile,
    /// The backend requires local image data (e.g. Anthropic base64).
    #[error("backend requires local image data")]
    RequiresLocalImage,
    /// Building the provider-specific image payload failed.
    #[error("failed to prepare image payload")]
    PayloadPrepFailed,
    /// A shared API/transport/parse failure.
    #[error(transparent)]
    Api(#[from] ClawApiError),
}

impl InferMediaError {
    /// Whether retrying the same request might succeed.
    #[must_use]
    pub fn is_retryable(&self) -> bool {
        matches!(self, InferMediaError::Api(err) if err.is_retryable())
    }
}
