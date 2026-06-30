//! Helpers shared by the LLM backends.

use serde_json::Value;

use super::super::backend::LlmBackend;
use super::super::errors::{ChatError, ClawApiError, InferMediaError};
use super::super::json_output::augment_system_with_schema;
use super::super::types::ModelProfile;
use super::super::types::{ChatJsonRequest, ChatRequest, LlmResponse, MediaAsset, ToolCall};
use claw_interface::http::{ClawHttp, HttpError};
use core::sync::atomic::AtomicBool;
use serde_json::Map;

/// HTTP statuses that indicate a transient, retryable server condition.
const STATUS_REQUEST_TIMEOUT: u16 = 408;
const STATUS_TOO_MANY_REQUESTS: u16 = 429;
const STATUS_SERVER_ERROR_MIN: u16 = 500;
const STATUS_SERVER_ERROR_MAX: u16 = 599;

/// Map a transport [`HttpError`] to a [`ClawApiError`], classifying whether the
/// failure is transient (retryable) or permanent. The retry decision is made by
/// the [`crate::ClawApi`] retry loop via [`ClawApiError::is_retryable`].
pub fn map_http_error(err: HttpError) -> ClawApiError {
    let message = err.to_string();
    if is_transient(&err) {
        ClawApiError::TransientTransport(message)
    } else {
        ClawApiError::Transport(message)
    }
}

fn is_transient(err: &HttpError) -> bool {
    match err {
        HttpError::Aborted | HttpError::InvalidUrl | HttpError::InvalidBody => false,
        HttpError::ClientInitFailed | HttpError::RequestFailed(_) => true,
        HttpError::UnexpectedStatus(message) => status_is_transient(message),
    }
}

/// Classify a non-200 status message. The transport bakes the status into the
/// message (e.g. `"HTTP 503: ..."`); treat an unparseable shape as transient.
fn status_is_transient(message: &str) -> bool {
    match parse_leading_status(message) {
        Some(code) => {
            code == STATUS_REQUEST_TIMEOUT
                || code == STATUS_TOO_MANY_REQUESTS
                || (STATUS_SERVER_ERROR_MIN..=STATUS_SERVER_ERROR_MAX).contains(&code)
        }
        None => true,
    }
}

/// Best-effort extraction of the first 3-digit HTTP status in `message`.
fn parse_leading_status(message: &str) -> Option<u16> {
    let bytes = message.as_bytes();
    let mut i = 0;
    while i + 3 <= bytes.len() {
        if bytes[i].is_ascii_digit() {
            let run_end = bytes[i..]
                .iter()
                .position(|b| !b.is_ascii_digit())
                .map(|p| i + p)
                .unwrap_or(bytes.len());
            if run_end - i == 3 {
                return message[i..run_end].parse::<u16>().ok();
            }
            i = run_end;
        } else {
            i += 1;
        }
    }
    None
}

/// `join_url` from the backends: join `base_url` and `path` with exactly one
/// slash between them.
pub fn join_url(base_url: &str, path: &str) -> String {
    let base_has_slash = base_url.ends_with('/');
    let path_has_slash = path.starts_with('/');
    if base_has_slash && path_has_slash {
        format!("{base_url}{}", &path[1..])
    } else if !base_has_slash && !path_has_slash {
        format!("{base_url}/{path}")
    } else {
        format!("{base_url}{path}")
    }
}

/// Parse an OpenAI chat-completions response, mirroring `parse_chat_response`
/// in `claw_llm_backend_openai_compatible.c`.
pub fn parse_openai_chat_response(body: &str) -> Result<LlmResponse, ClawApiError> {
    let root: Value = serde_json::from_str(body).map_err(|_| ClawApiError::Parse)?;

    let message = root
        .get("choices")
        .and_then(|c| c.as_array())
        .and_then(|a| a.first())
        .and_then(|c0| c0.get("message"));
    let message = match message {
        Some(m) if m.is_object() => m,
        _ => return Err(ClawApiError::MalformedResponse("response missing message")),
    };

    if message.get("role").and_then(|r| r.as_str()) != Some("assistant") {
        return Err(ClawApiError::MalformedResponse(
            "response message is not assistant",
        ));
    }

    let raw_message_json = serde_json::to_string(message)
        .map_err(|_| ClawApiError::ApiError("out of memory copying raw message"))?;

    let text = message
        .get("content")
        .and_then(|c| c.as_str())
        .filter(|s| !s.is_empty())
        .map(|s| s.to_string());

    // reasoning_content: kept even when empty, as long as it is a string.
    let reasoning_content = message
        .get("reasoning_content")
        .and_then(|r| r.as_str())
        .map(|s| s.to_string());

    let mut tool_calls = Vec::new();
    if let Some(arr) = message.get("tool_calls").and_then(|t| t.as_array()) {
        for tc in arr {
            let function = tc.get("function");
            let id = tc.get("id");
            let name = function.and_then(|f| f.get("name"));
            let args = function.and_then(|f| f.get("arguments"));
            if id.is_none() || function.is_none() || name.is_none() || args.is_none() {
                return Err(ClawApiError::MalformedResponse("malformed tool call"));
            }
            match (
                id.unwrap().as_str(),
                name.unwrap().as_str(),
                args.unwrap().as_str(),
            ) {
                (Some(id), Some(name), Some(args)) => tool_calls.push(ToolCall {
                    id: id.to_string(),
                    name: name.to_string(),
                    arguments_json: args.to_string(),
                }),
                _ => return Err(ClawApiError::MalformedResponse("malformed tool call")),
            }
        }
    }

    if text.is_none() && tool_calls.is_empty() {
        return Err(ClawApiError::EmptyResponse);
    }

    Ok(LlmResponse {
        text,
        reasoning_content,
        raw_message_json: Some(raw_message_json),
        tool_calls,
    })
}

/// Insert OpenAI-style `tools` into a chat request body map.
pub fn insert_tools_into_body(
    body: &mut Map<String, Value>,
    profile: &ModelProfile,
    tools_json: &str,
) -> Result<(), ChatError> {
    if !profile.supports_tools {
        return Err(ChatError::ToolsUnsupported);
    }
    let tools: Value = serde_json::from_str(tools_json).map_err(|_| ChatError::InvalidToolsJson)?;
    if !tools.is_array() {
        return Err(ChatError::InvalidToolsJson);
    }
    body.insert("tools".to_string(), tools);
    Ok(())
}

/// Prompt-fallback structured chat: inject schema into system prompt, then `chat`.
pub fn chat_json_prompt_fallback<H: ClawHttp>(
    backend: &dyn LlmBackend<H>,
    http: &mut H,
    profile: &ModelProfile,
    request: &ChatJsonRequest<'_>,
    schema: &Value,
    abort: &AtomicBool,
) -> Result<LlmResponse, ChatError> {
    let system = augment_system_with_schema(request.system_prompt, schema);
    let mut chat_req =
        ChatRequest::new(&system, request.messages).with_reminders(request.reminders);
    if let Some(tools_json) = request.tools_json.filter(|s| !s.is_empty()) {
        chat_req = chat_req.with_tools(tools_json);
    }
    backend.chat(http, profile, &chat_req, abort)
}

/// Select the single media asset a backend will send.
///
/// An empty asset list is a returnable [`InferMediaError::IncompleteRequest`].
/// Sending more than one asset in a single request is not implemented yet, so
/// that path is left explicitly unimplemented rather than silently dropping the
/// extra assets.
pub fn single_media_asset(media: &[MediaAsset]) -> Result<&MediaAsset, InferMediaError> {
    match media {
        [] => Err(InferMediaError::IncompleteRequest),
        [asset] => Ok(asset),
        _ => unimplemented!("multiple media assets per request is not supported yet"),
    }
}
