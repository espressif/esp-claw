//! Backend selection and dispatch.
//!
//! The original C runtime used a vtable because backend implementations were
//! selected by string at runtime. In Rust the same runtime choice is a closed,
//! built-in enum, while HTTP transport dispatch stays generic/static at the call
//! site.
//!
//! Registering a backend is a single line in the [`define_backends!`] table plus
//! a [`BackendImpl`] in its module. Wire details and capability flags live on the
//! trait (as associated consts), so a backend owns its own metadata instead of
//! duplicating it in a table here.

mod anthropic;
mod openai_compatible;
mod shared;
pub(crate) mod sse;

use core::{fmt, str::FromStr, sync::atomic::AtomicBool};

use claw_interface::http::{
    blocking::ClawHttp as BlockingClawHttp, Cancel, ClawHttp, StreamingHttp,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use super::errors::{ChatError, InferMediaError, InitError};
use super::stream::ChatStream;
use super::types::{ChatJsonRequest, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest};

/// Failed to parse a string backend id into [`BackendKind`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParseBackendKindError;

impl fmt::Display for ParseBackendKindError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("unknown LLM backend type")
    }
}

impl std::error::Error for ParseBackendKindError {}

/// Behavior each built-in backend implements.
///
/// This is a pure behavioral contract: `make` plus the request methods. Wire
/// details (endpoint path, provider field names, media-input rules) are
/// backend-internal and live as private constants in each backend module, not
/// here. The trait is crate-internal and never used as `dyn` (its request
/// methods are generic over the HTTP transport), so [`Backend`] erases the
/// concrete backend behind a small enum instead.
trait BackendImpl: Sized {
    /// Build the backend from validated config.
    ///
    /// Credential/config validation is centralized in [`crate::ClawApi::set_config`];
    /// `api_key`, `model`, and `base_url` are guaranteed non-empty here.
    fn make(config: &ClawApiConfig) -> Result<Self, InitError>;

    fn chat<H: BlockingClawHttp>(
        &self,
        http: &mut H,
        request: &ChatRequest<'_>,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError>;

    fn chat_json<H: BlockingClawHttp>(
        &self,
        http: &mut H,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError>;

    fn infer_media<H: BlockingClawHttp>(
        &self,
        http: &mut H,
        request: &MediaRequest<'_>,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError>;

    async fn chat_async<H: ClawHttp>(
        &self,
        http: &mut H,
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError>;

    async fn chat_json_async<H: ClawHttp>(
        &self,
        http: &mut H,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError>;

    async fn infer_media_async<H: ClawHttp>(
        &self,
        http: &mut H,
        request: &MediaRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<String, InferMediaError>;

    /// Streaming chat completion over [`StreamingHttp`]. Builds a `stream: true`
    /// request, and on 2xx wraps the response body stream in a [`ChatStream`]
    /// backed by this backend's SSE parser; a non-2xx status reads the error body
    /// and fails.
    async fn chat_stream_async<'a, 'r, H: StreamingHttp>(
        &'a self,
        http: &'a mut H,
        request: &'r ChatRequest<'r>,
        cancel: Cancel<'a>,
    ) -> Result<ChatStream<H::ByteStream>, ChatError>;
}

/// Constructed backend instance, dispatched by [`BackendKind`].
pub(crate) struct Backend(BackendInner);

/// Declare the closed set of built-in backends in one place.
///
/// Each entry is `Variant => Type, "id"`. The macro generates [`BackendKind`],
/// the private `BackendInner` storage enum, the [`Backend`] transport dispatch,
/// and the id `as_str`/`FromStr`/serde mapping.
macro_rules! define_backends {
    ( $( $variant:ident => $backend:ty, $id:literal );+ $(;)? ) => {
        /// Built-in backend kind selected by [`ClawApiConfig`](crate::ClawApiConfig).
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
        pub enum BackendKind {
            $(
                #[serde(rename = $id)]
                $variant,
            )+
        }

        enum BackendInner {
            $( $variant($backend), )+
        }

        impl BackendKind {
            /// The stable string id of this backend (config + logs).
            #[must_use]
            pub fn as_str(self) -> &'static str {
                match self {
                    $( Self::$variant => $id, )+
                }
            }

            pub(crate) fn make(self, config: &ClawApiConfig) -> Result<Backend, InitError> {
                Ok(Backend(match self {
                    $( Self::$variant =>
                        BackendInner::$variant(<$backend as BackendImpl>::make(config)?), )+
                }))
            }
        }

        impl FromStr for BackendKind {
            type Err = ParseBackendKindError;

            fn from_str(id: &str) -> Result<Self, Self::Err> {
                match id {
                    $( $id => Ok(Self::$variant), )+
                    _ => Err(ParseBackendKindError),
                }
            }
        }

        impl Backend {
            pub(crate) fn chat<H: BlockingClawHttp>(
                &self,
                http: &mut H,
                request: &ChatRequest<'_>,
                abort: &AtomicBool,
            ) -> Result<LlmResponse, ChatError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::chat(backend, http, request, abort), )+
                }
            }

            pub(crate) fn chat_json<H: BlockingClawHttp>(
                &self,
                http: &mut H,
                request: &ChatJsonRequest<'_>,
                schema_name: &str,
                schema: &Value,
                abort: &AtomicBool,
            ) -> Result<LlmResponse, ChatError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::chat_json(
                            backend, http, request, schema_name, schema, abort,
                        ), )+
                }
            }

            pub(crate) fn infer_media<H: BlockingClawHttp>(
                &self,
                http: &mut H,
                request: &MediaRequest<'_>,
                abort: &AtomicBool,
            ) -> Result<String, InferMediaError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::infer_media(backend, http, request, abort), )+
                }
            }

            pub(crate) async fn chat_async<H: ClawHttp>(
                &self,
                http: &mut H,
                request: &ChatRequest<'_>,
                cancel: Cancel<'_>,
            ) -> Result<LlmResponse, ChatError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::chat_async(backend, http, request, cancel).await, )+
                }
            }

            pub(crate) async fn chat_json_async<H: ClawHttp>(
                &self,
                http: &mut H,
                request: &ChatJsonRequest<'_>,
                schema_name: &str,
                schema: &Value,
                cancel: Cancel<'_>,
            ) -> Result<LlmResponse, ChatError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::chat_json_async(
                            backend, http, request, schema_name, schema, cancel,
                        )
                        .await, )+
                }
            }

            pub(crate) async fn infer_media_async<H: ClawHttp>(
                &self,
                http: &mut H,
                request: &MediaRequest<'_>,
                cancel: Cancel<'_>,
            ) -> Result<String, InferMediaError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::infer_media_async(backend, http, request, cancel)
                            .await, )+
                }
            }

            pub(crate) async fn chat_stream_async<'a, 'r, H: StreamingHttp>(
                &'a self,
                http: &'a mut H,
                request: &'r ChatRequest<'r>,
                cancel: Cancel<'a>,
            ) -> Result<ChatStream<H::ByteStream>, ChatError> {
                match &self.0 {
                    $( BackendInner::$variant(backend) =>
                        BackendImpl::chat_stream_async(backend, http, request, cancel)
                            .await, )+
                }
            }
        }
    };
}

define_backends! {
    OpenAiCompatible => openai_compatible::OpenAiCompatible, "openai_compatible";
    AnthropicCompatible => anthropic::Anthropic, "anthropic_compatible";
}

impl fmt::Display for BackendKind {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}
