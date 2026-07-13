//! Error surface: trigger the real [`InitError`] variants, then enumerate every
//! variant of [`ClawApiError`], [`ChatError`], [`ChatJsonError`], and
//! [`InferMediaError`] and report [`is_retryable`](ClawApiError::is_retryable).
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-api --example errors --target x86_64-unknown-linux-gnu
//! ```
//!
//! Every arm names its variant explicitly, so the exhaustive `match`es fail to
//! compile if the public error surface changes.

use std::sync::atomic::AtomicBool;

use claw_api::{
    BackendKind, ChatError, ChatJsonError, ClawApi, ClawApiConfig, ClawApiError, InferMediaError,
    InitError,
};
use claw_interface::http::{
    blocking::ClawHttp, HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode,
};

/// Never actually reached: `init` validates config before touching transport.
struct UnusedHttp;
impl ClawHttp for UnusedHttp {
    fn post_json(
        &mut self,
        _request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: String::new(),
        })
    }
}

/// Real `InitError`s from config validation (transport is never called).
fn show_init_errors() {
    let cases = [
        ("", "m", "u"), // empty api key
        ("k", "", "u"), // empty model
        ("k", "m", ""), // empty base url
    ];
    for (api_key, model, base_url) in cases {
        let config = ClawApiConfig::new(BackendKind::OpenAiCompatible, api_key, model, base_url);
        let mut api = ClawApi::new(UnusedHttp);
        let label = match api.set_config(config) {
            Ok(()) => "ok (unexpected)".to_string(),
            Err(InitError::MissingApiKey) => "MissingApiKey".to_string(),
            Err(InitError::MissingModel) => "MissingModel".to_string(),
            Err(InitError::MissingBaseUrl) => "MissingBaseUrl".to_string(),
        };
        println!("init       -> {label}");
    }
}

/// Every [`ClawApiError`] variant, named so the match stays exhaustive.
fn api_error_label(error: &ClawApiError) -> &'static str {
    match error {
        ClawApiError::NotConfigured => "NotConfigured",
        ClawApiError::Transport(_) => "Transport",
        ClawApiError::TransientTransport(_) => "TransientTransport",
        ClawApiError::Parse => "Parse",
        ClawApiError::EmptyResponse => "EmptyResponse",
        ClawApiError::MalformedResponse(_) => "MalformedResponse",
        ClawApiError::ApiError(_) => "ApiError",
    }
}

fn chat_error_label(error: &ChatError) -> &'static str {
    match error {
        ChatError::InvalidToolsJson => "InvalidToolsJson",
        ChatError::Api(_) => "Api",
    }
}

fn chat_json_error_label(error: &ChatJsonError) -> &'static str {
    match error {
        ChatJsonError::EmptyText => "EmptyText",
        ChatJsonError::InvalidOutput(_) => "InvalidOutput",
        ChatJsonError::MissingOutputSchema => "MissingOutputSchema",
        ChatJsonError::Chat(_) => "Chat",
    }
}

fn infer_media_error_label(error: &InferMediaError) -> &'static str {
    match error {
        InferMediaError::IncompleteRequest => "IncompleteRequest",
        InferMediaError::MultipleMediaAssetsUnsupported => "MultipleMediaAssetsUnsupported",
        InferMediaError::MediaPathEmpty => "MediaPathEmpty",
        InferMediaError::MediaPathNotAbsolute => "MediaPathNotAbsolute",
        InferMediaError::MediaUrlEmpty => "MediaUrlEmpty",
        InferMediaError::UnsupportedMediaType => "UnsupportedMediaType",
        InferMediaError::MediaNotFound => "MediaNotFound",
        InferMediaError::MediaFileEmpty => "MediaFileEmpty",
        InferMediaError::MediaTooLarge => "MediaTooLarge",
        InferMediaError::MediaReadFailed => "MediaReadFailed",
        InferMediaError::UnsupportedMediaKind => "UnsupportedMediaKind",
        InferMediaError::RemoteOnlyProfile => "RemoteOnlyProfile",
        InferMediaError::RequiresLocalImage => "RequiresLocalImage",
        InferMediaError::PayloadPrepFailed => "PayloadPrepFailed",
        InferMediaError::Api(_) => "Api",
    }
}

fn main() {
    show_init_errors();

    // Shared transport/parse failures, and their retry classification.
    let api_errors = [
        ClawApiError::Transport("connection reset".into()),
        ClawApiError::TransientTransport("HTTP 503".into()),
        ClawApiError::Parse,
        ClawApiError::EmptyResponse,
        ClawApiError::MalformedResponse("missing choices"),
        ClawApiError::ApiError("out of memory"),
    ];
    for error in &api_errors {
        println!(
            "api_err    -> {:<18} retryable={} :: {error}",
            api_error_label(error),
            error.is_retryable(),
        );
    }

    // Chat errors: the tool-JSON case plus a wrapped transient transport failure.
    let chat_errors = [
        ChatError::InvalidToolsJson,
        ChatError::Api(ClawApiError::TransientTransport("HTTP 429".into())),
    ];
    for error in &chat_errors {
        println!(
            "chat_err   -> {:<18} retryable={} :: {error}",
            chat_error_label(error),
            error.is_retryable(),
        );
    }

    // Structured-JSON errors, including a retryable wrapped chat failure.
    let chat_json_errors = [
        ChatJsonError::EmptyText,
        ChatJsonError::InvalidOutput("expected integer".into()),
        ChatJsonError::MissingOutputSchema,
        ChatJsonError::Chat(ChatError::Api(ClawApiError::TransientTransport(
            "HTTP 500".into(),
        ))),
    ];
    for error in &chat_json_errors {
        println!(
            "json_err   -> {:<18} retryable={} :: {error}",
            chat_json_error_label(error),
            error.is_retryable(),
        );
    }

    // Every media-pipeline failure mode.
    let media_errors = [
        InferMediaError::IncompleteRequest,
        InferMediaError::MultipleMediaAssetsUnsupported,
        InferMediaError::MediaPathEmpty,
        InferMediaError::MediaPathNotAbsolute,
        InferMediaError::MediaUrlEmpty,
        InferMediaError::UnsupportedMediaType,
        InferMediaError::MediaNotFound,
        InferMediaError::MediaFileEmpty,
        InferMediaError::MediaTooLarge,
        InferMediaError::MediaReadFailed,
        InferMediaError::UnsupportedMediaKind,
        InferMediaError::RemoteOnlyProfile,
        InferMediaError::RequiresLocalImage,
        InferMediaError::PayloadPrepFailed,
        InferMediaError::Api(ClawApiError::Transport("io".into())),
    ];
    for error in &media_errors {
        println!(
            "media_err  -> {:<30} retryable={}",
            infer_media_error_label(error),
            error.is_retryable(),
        );
    }
}
