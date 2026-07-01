//! Backend selection and dispatch.
//!
//! The original C runtime used a vtable because backend implementations were
//! selected by string at runtime. In Rust the same runtime choice is simpler as
//! an enum: the backend set is closed and built in, while HTTP transport
//! dispatch remains generic/static at the call site.

mod anthropic;
mod openai_compatible;
mod shared;

use core::{fmt, str::FromStr, sync::atomic::AtomicBool};

use claw_interface::http::{Cancel, ClawHttp, ClawHttpAsync};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use super::errors::{ChatError, InferMediaError, InitError};
use super::types::{
    ChatJsonRequest, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest, ModelProfile,
};

/// `claw_llm_backend_defaults_t`
#[derive(Clone, Copy)]
struct BackendDefaults {
    chat_path: &'static str,
    max_tokens_field: &'static str,
    supports_tools: bool,
    supports_vision: bool,
    supports_json_schema: bool,
    image_remote_url_only: bool,
}

/// Built-in backend kind selected by [`ClawApiConfig`](crate::ClawApiConfig).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum BackendKind {
    #[serde(rename = "openai_compatible")]
    OpenAiCompatible,
    #[serde(rename = "anthropic_compatible")]
    AnthropicCompatible,
}

/// Failed to parse a string backend id into [`BackendKind`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParseBackendKindError;

impl fmt::Display for ParseBackendKindError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("unknown LLM backend type")
    }
}

impl std::error::Error for ParseBackendKindError {}

impl BackendKind {
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::OpenAiCompatible => openai_compatible::ID,
            Self::AnthropicCompatible => anthropic::ID,
        }
    }

    fn defaults(self) -> BackendDefaults {
        match self {
            Self::OpenAiCompatible => BackendDefaults {
                chat_path: openai_compatible::CHAT_PATH,
                max_tokens_field: openai_compatible::DEFAULT_MAX_TOKENS_FIELD,
                supports_tools: true,
                supports_vision: true,
                supports_json_schema: true,
                image_remote_url_only: false,
            },
            Self::AnthropicCompatible => BackendDefaults {
                chat_path: anthropic::CHAT_PATH,
                max_tokens_field: anthropic::DEFAULT_MAX_TOKENS_FIELD,
                supports_tools: true,
                supports_vision: true,
                supports_json_schema: true,
                image_remote_url_only: false,
            },
        }
    }

    pub(crate) fn profile(self) -> ModelProfile {
        let defaults = self.defaults();
        ModelProfile::new(
            defaults.chat_path,
            defaults.max_tokens_field,
            defaults.supports_tools,
            defaults.supports_vision,
            defaults.supports_json_schema,
            defaults.image_remote_url_only,
        )
    }

    pub(crate) fn make(self, config: &ClawApiConfig) -> Result<Backend, InitError> {
        match self {
            Self::OpenAiCompatible => Ok(Backend(BackendInner::OpenAi(
                openai_compatible::OpenAiCompatible::make(config)?,
            ))),
            Self::AnthropicCompatible => Ok(Backend(BackendInner::Anthropic(
                anthropic::Anthropic::make(config)?,
            ))),
        }
    }
}

impl fmt::Display for BackendKind {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

impl FromStr for BackendKind {
    type Err = ParseBackendKindError;

    fn from_str(id: &str) -> Result<Self, Self::Err> {
        match id {
            openai_compatible::ID => Ok(Self::OpenAiCompatible),
            anthropic::ID => Ok(Self::AnthropicCompatible),
            _ => Err(ParseBackendKindError),
        }
    }
}

trait BackendImpl {
    fn chat<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest<'_>,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError>;

    fn chat_json<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError>;

    fn infer_media<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest<'_>,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError>;

    async fn chat_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError>;

    async fn chat_json_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError>;

    async fn infer_media_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<String, InferMediaError>;
}

/// Constructed backend instance.
pub(crate) struct Backend(BackendInner);

enum BackendInner {
    OpenAi(openai_compatible::OpenAiCompatible),
    Anthropic(anthropic::Anthropic),
}

impl Backend {
    pub(crate) fn chat<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        match &self.0 {
            BackendInner::OpenAi(backend) => {
                BackendImpl::chat(backend, http, profile, request, abort)
            }
            BackendInner::Anthropic(backend) => {
                BackendImpl::chat(backend, http, profile, request, abort)
            }
        }
    }

    /// Structured JSON chat. OpenAI uses API `response_format` when supported;
    /// Anthropic and others use prompt fallback until API-level support lands.
    pub(crate) fn chat_json<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        match &self.0 {
            BackendInner::OpenAi(backend) => {
                BackendImpl::chat_json(backend, http, profile, request, schema_name, schema, abort)
            }
            BackendInner::Anthropic(backend) => {
                BackendImpl::chat_json(backend, http, profile, request, schema_name, schema, abort)
            }
        }
    }

    pub(crate) fn infer_media<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError> {
        match &self.0 {
            BackendInner::OpenAi(backend) => {
                BackendImpl::infer_media(backend, http, profile, request, abort)
            }
            BackendInner::Anthropic(backend) => {
                BackendImpl::infer_media(backend, http, profile, request, abort)
            }
        }
    }

    pub(crate) async fn chat_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        match &self.0 {
            BackendInner::OpenAi(backend) => {
                BackendImpl::chat_async(backend, http, profile, request, cancel).await
            }
            BackendInner::Anthropic(backend) => {
                BackendImpl::chat_async(backend, http, profile, request, cancel).await
            }
        }
    }

    pub(crate) async fn chat_json_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        match &self.0 {
            BackendInner::OpenAi(backend) => {
                BackendImpl::chat_json_async(
                    backend,
                    http,
                    profile,
                    request,
                    schema_name,
                    schema,
                    cancel,
                )
                .await
            }
            BackendInner::Anthropic(backend) => {
                BackendImpl::chat_json_async(
                    backend,
                    http,
                    profile,
                    request,
                    schema_name,
                    schema,
                    cancel,
                )
                .await
            }
        }
    }

    pub(crate) async fn infer_media_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<String, InferMediaError> {
        match &self.0 {
            BackendInner::OpenAi(backend) => {
                BackendImpl::infer_media_async(backend, http, profile, request, cancel).await
            }
            BackendInner::Anthropic(backend) => {
                BackendImpl::infer_media_async(backend, http, profile, request, cancel).await
            }
        }
    }
}
