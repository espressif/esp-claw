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
//! use claw_api::{BackendKind, ChatRequest, ClawApi, ClawApiConfig, RetryPolicy};
//! use claw_interface::http::{blocking::ClawHttp, HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode};
//!
//! // 1. Provide an HTTP transport. On device this wraps `esp_http_client`;
//! //    here we stub a fixed OpenAI-shaped reply.
//! struct MyHttp;
//! impl ClawHttp for MyHttp {
//!     fn post_json(&mut self, _req: &HttpJsonRequest, _abort: &AtomicBool)
//!         -> Result<HttpResponse, HttpError> {
//!         Ok(HttpResponse {
//!             status_code: HttpStatusCode::OK,
//!             body: r#"{"choices":[{"message":{"role":"assistant","content":"Hi!"}}]}"#.into(),
//!         })
//!     }
//! }
//!
//! // 2. Build the client once. It owns the transport and is driven via `&mut`.
//! let config = ClawApiConfig::new(
//!     BackendKind::OpenAiCompatible,
//!     "sk-...",
//!     "gpt-4o-mini",
//!     "https://api.openai.com/v1",
//! );
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

#![cfg_attr(
    not(test),
    forbid(
        clippy::arithmetic_side_effects,
        clippy::expect_used,
        clippy::indexing_slicing,
        clippy::panic,
        clippy::unwrap_used
    )
)]
#![cfg_attr(
    not(test),
    warn(clippy::todo, clippy::unimplemented, clippy::unreachable)
)]

// Implementation modules are private: the public surface is the curated
// re-exports below. The backend registry, media-prep pipeline, prompt helpers,
// and retry loop are internal details, not part of the end-user API.
mod backends;
mod client;
mod errors;
mod media;
mod retry;
mod stream;
mod types;

pub use backends::{BackendKind, ParseBackendKindError};
pub use client::{ClawApi, ClawApiAsync};
pub use errors::{ChatError, ChatJsonError, ClawApiError, InferMediaError, InitError};
pub use stream::ChatStream;
#[cfg(feature = "cache_profile")]
pub use types::ApiUsage;
pub use types::{
    ChatJsonRequest, ChatJsonResponse, ChatRequest, ClawApiConfig, LlmDelta, LlmResponse,
    MediaAsset, MediaRequest, RetryPolicy, StaticOutputSchema, ToolCall,
};
