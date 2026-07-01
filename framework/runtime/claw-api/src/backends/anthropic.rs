//! Anthropic-compatible backend, port of `claw_llm_backend_anthropic.c`.
//!
//! Converts OpenAI-style messages/tools to the Anthropic Messages API shape and
//! parses the Anthropic content-block response back into a [`LlmResponse`].
//!
//! Structured JSON ([`crate::ClawApi::chat_json`]) uses Anthropic
//! `output_config.format` when [`crate::ModelProfile::supports_json_schema`]
//! is set; otherwise it falls back to schema-in-prompt.

use core::sync::atomic::AtomicBool;

use serde_json::{json, Map, Value};

use claw_interface::http::{Cancel, ClawHttp, ClawHttpAsync, HttpAuth, HttpHeader};

use super::super::errors::{ChatError, ClawApiError, InferMediaError, InitError};
use super::super::media::prepare_asset;
use super::super::types::{
    ChatJsonRequest, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest, ModelProfile, ToolCall,
};
use super::shared::{
    post_json, post_json_async, single_media_asset, BackendContext, ChatJsonPromptFallback,
};
use super::BackendImpl;

pub(super) const ID: &str = "anthropic_compatible";
pub(super) const CHAT_PATH: &str = "/messages";
pub(super) const DEFAULT_MAX_TOKENS_FIELD: &str = "max_tokens";
const ANTHROPIC_VERSION: &str = "2023-06-01";

pub(super) struct Anthropic {
    context: BackendContext,
}

/// `anthropic_init`
///
/// Credential/config validation is centralized in [`crate::ClawApi::init`];
/// `api_key`, `model`, and `base_url` are guaranteed non-empty here.
fn str_field<'a>(obj: &'a Value, key: &str) -> Option<&'a str> {
    obj.get(key).and_then(|v| v.as_str())
}

/// `anthropic_make_text_block`
fn make_text_block(text: &str) -> Option<Value> {
    if text.is_empty() {
        return None;
    }
    Some(json!({"type": "text", "text": text}))
}

/// `anthropic_make_tool_use_block`
fn make_tool_use_block(tool_call: &Value) -> Option<Value> {
    if !tool_call.is_object() {
        return None;
    }
    let id = str_field(tool_call, "id")?;
    let function = tool_call.get("function");
    let name = function
        .and_then(|f| f.get("name"))
        .and_then(|n| n.as_str())?;
    let args = function
        .and_then(|f| f.get("arguments"))
        .and_then(|a| a.as_str());
    let input = match args {
        Some(s) if !s.is_empty() => serde_json::from_str::<Value>(s).unwrap_or_else(|_| json!({})),
        _ => json!({}),
    };
    Some(json!({"type": "tool_use", "id": id, "name": name, "input": input}))
}

/// `anthropic_duplicate_supported_block`
fn duplicate_supported_block(block: &Value) -> Option<Value> {
    let ty = str_field(block, "type")?;
    matches!(
        ty,
        "text" | "tool_use" | "tool_result" | "thinking" | "redacted_thinking"
    )
    .then(|| block.clone())
}

/// `convert_messages_to_anthropic`
///
/// Converts the persisted `messages` history followed by the ephemeral
/// `reminders` (a two-segment tail) into the Anthropic message shape. The two
/// segments are viewed as one sequence of references (no `Value` is cloned to
/// fuse them) so consecutive-tool-message merging still works across the seam.
fn convert_messages_to_anthropic(
    messages: &Value,
    reminders: &[Value],
) -> Result<Value, ClawApiError> {
    let mut out: Vec<Value> = Vec::new();
    let history = match messages.as_array() {
        Some(a) => a.as_slice(),
        None => &[],
    };
    let mut iter = history.iter().chain(reminders.iter()).peekable();

    while let Some(msg) = iter.next() {
        let role = match str_field(msg, "role") {
            Some(r) if !r.is_empty() => r,
            _ => continue,
        };

        // Merge consecutive "tool"-role messages into one "user" message.
        if role == "tool" {
            let mut tool_blocks: Vec<Value> = Vec::new();
            push_tool_result_block(msg, &mut tool_blocks);
            while iter
                .peek()
                .is_some_and(|next| str_field(next, "role") == Some("tool"))
            {
                let Some(inner) = iter.next() else {
                    break;
                };
                push_tool_result_block(inner, &mut tool_blocks);
            }
            out.push(json!({"role": "user", "content": tool_blocks}));
            continue;
        }

        if role != "assistant" && role != "user" {
            continue;
        }

        let mut blocks: Vec<Value> = Vec::new();
        let content = msg.get("content");
        match content {
            Some(Value::String(s)) if !s.is_empty() => {
                if let Some(b) = make_text_block(s) {
                    blocks.push(b);
                }
            }
            Some(Value::Array(items)) => {
                for block in items {
                    if let Some(dup) = duplicate_supported_block(block) {
                        blocks.push(dup);
                    }
                }
            }
            _ => {}
        }

        if role == "assistant" {
            if let Some(reasoning) = str_field(msg, "reasoning_content").filter(|s| !s.is_empty()) {
                blocks.insert(0, json!({"type": "thinking", "thinking": reasoning}));
            }
            if let Some(tool_calls) = msg.get("tool_calls").and_then(|t| t.as_array()) {
                for tc in tool_calls {
                    match make_tool_use_block(tc) {
                        Some(b) => blocks.push(b),
                        None => {
                            return Err(ClawApiError::ApiError("out of memory converting messages"))
                        }
                    }
                }
            }
        }

        if blocks.is_empty() {
            continue;
        }

        out.push(json!({"role": role, "content": blocks}));
    }

    Ok(Value::Array(out))
}

fn push_tool_result_block(message: &Value, tool_blocks: &mut Vec<Value>) {
    let tid = str_field(message, "tool_call_id").unwrap_or("");
    let content = str_field(message, "content").unwrap_or("");
    let is_error = message
        .get("is_error")
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    tool_blocks.push(json!({
        "type": "tool_result",
        "tool_use_id": tid,
        "content": content,
        "is_error": is_error,
    }));
}

/// `convert_tools_to_anthropic`. Returns `None` when there are no tools or the
/// JSON is invalid (the caller distinguishes the two).
///
/// When `strict` is true, each tool gets `"strict": true` for Anthropic structured
/// outputs combined with strict tool use.
fn convert_tools_to_anthropic(tools_json: Option<&str>, strict: bool) -> Option<Value> {
    let tools_json = tools_json.filter(|s| !s.is_empty())?;
    let parsed: Value = serde_json::from_str(tools_json).ok()?;
    let arr = parsed.as_array()?;

    let mut out: Vec<Value> = Vec::new();
    for item in arr {
        let (name, desc, schema) = if item.is_object() {
            if str_field(item, "type") == Some("function") {
                let function = item.get("function");
                (
                    function.and_then(|f| f.get("name")),
                    function.and_then(|f| f.get("description")),
                    function.and_then(|f| f.get("parameters")),
                )
            } else {
                (
                    item.get("name"),
                    item.get("description"),
                    item.get("input_schema"),
                )
            }
        } else {
            (None, None, None)
        };

        let name = match name.and_then(|n| n.as_str()).filter(|s| !s.is_empty()) {
            Some(n) => n,
            None => continue,
        };

        let mut tool = Map::new();
        tool.insert("name".to_string(), json!(name));
        if let Some(d) = desc.and_then(|d| d.as_str()) {
            tool.insert("description".to_string(), json!(d));
        }
        match schema {
            Some(s) => tool.insert("input_schema".to_string(), s.clone()),
            None => tool.insert("input_schema".to_string(), json!({})),
        };
        if strict {
            tool.insert("strict".to_string(), json!(true));
        }
        out.push(Value::Object(tool));
    }

    Some(Value::Array(out))
}

/// `parse_data_url`: split `data:<mime>;base64,<data>`.
fn parse_data_url(data_url: &str) -> Option<(String, String)> {
    const PREFIX: &str = "data:";
    const MARKER: &str = ";base64,";
    let rest = data_url.strip_prefix(PREFIX)?;
    let (mime, data) = rest.split_once(MARKER)?;
    if data.is_empty() {
        return None;
    }
    Some((mime.to_string(), data.to_string()))
}

/// `parse_chat_response` (Anthropic content-block form).
fn parse_chat_response(body: &str) -> Result<LlmResponse, ClawApiError> {
    let root: Value = serde_json::from_str(body).map_err(|_| ClawApiError::Parse)?;
    let content = match root.get("content") {
        Some(Value::Array(a)) => a,
        _ => return Err(ClawApiError::MalformedResponse("response missing content")),
    };

    let raw_message_json = serde_json::to_string(&json!({
        "role": "assistant",
        "content": Value::Array(content.clone()),
    }))
    .map_err(|_| ClawApiError::ApiError("out of memory copying raw message"))?;

    let mut text = String::new();
    let mut reasoning = String::new();
    let mut tool_calls: Vec<ToolCall> = Vec::new();

    for block in content {
        match str_field(block, "type") {
            Some("text") => {
                if let Some(t) = str_field(block, "text") {
                    text.push_str(t);
                }
            }
            Some("thinking") => {
                if let Some(t) = str_field(block, "thinking") {
                    reasoning.push_str(t);
                }
            }
            Some("tool_use") => {
                let id = str_field(block, "id")
                    .ok_or(ClawApiError::MalformedResponse("malformed tool call"))?;
                let name = str_field(block, "name")
                    .ok_or(ClawApiError::MalformedResponse("malformed tool call"))?;
                let arguments_json = match block.get("input") {
                    Some(input) => serde_json::to_string(input)
                        .map_err(|_| ClawApiError::ApiError("out of memory copying tool call"))?,
                    None => "{}".to_string(),
                };
                tool_calls.push(ToolCall {
                    id: id.to_string(),
                    name: name.to_string(),
                    arguments_json,
                });
            }
            _ => {}
        }
    }

    let text_opt = (!text.is_empty()).then_some(text);
    let reasoning_opt = (!reasoning.is_empty()).then_some(reasoning);

    if text_opt.is_none() && tool_calls.is_empty() && reasoning_opt.is_none() {
        return Err(ClawApiError::EmptyResponse);
    }

    Ok(LlmResponse {
        text: text_opt,
        reasoning_content: reasoning_opt,
        raw_message_json: Some(raw_message_json),
        tool_calls,
    })
}

impl Anthropic {
    pub(super) fn make(config: &ClawApiConfig) -> Result<Self, InitError> {
        Ok(Anthropic {
            context: BackendContext::from_config(config),
        })
    }

    /// `build_chat_body`
    fn build_chat_body(&self, request: &ChatRequest) -> Result<String, ChatError> {
        let messages = convert_messages_to_anthropic(request.messages, request.reminders)?;

        let mut body = Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert("max_tokens".to_string(), json!(self.context.max_tokens()));
        if !request.system_prompt.is_empty() {
            body.insert("system".to_string(), json!(request.system_prompt));
        }
        body.insert("messages".to_string(), messages);

        Self::insert_tools_into_body(&mut body, request.tools_json, false)?;

        serde_json::to_string(&Value::Object(body)).map_err(|_| {
            ChatError::Api(ClawApiError::ApiError("out of memory serializing request"))
        })
    }

    fn build_chat_json_body(
        &self,
        request: &ChatJsonRequest<'_>,
        schema: &Value,
    ) -> Result<String, ChatError> {
        let messages = convert_messages_to_anthropic(request.messages, request.reminders)?;

        let mut body = Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert("max_tokens".to_string(), json!(self.context.max_tokens()));
        if !request.system_prompt.is_empty() {
            body.insert("system".to_string(), json!(request.system_prompt));
        }
        body.insert("messages".to_string(), messages);
        body.insert(
            "output_config".to_string(),
            json!({
                "format": {
                    "type": "json_schema",
                    "schema": schema,
                }
            }),
        );

        Self::insert_tools_into_body(&mut body, request.tools_json, true)?;

        serde_json::to_string(&Value::Object(body)).map_err(|_| {
            ChatError::Api(ClawApiError::ApiError("out of memory serializing request"))
        })
    }

    fn insert_tools_into_body(
        body: &mut Map<String, Value>,
        tools_json: Option<&str>,
        strict: bool,
    ) -> Result<(), ChatError> {
        let tools = convert_tools_to_anthropic(tools_json, strict);
        if tools_json.map(|s| !s.is_empty()).unwrap_or(false) && tools.is_none() {
            return Err(ChatError::InvalidToolsJson);
        }
        if let Some(tools) = tools {
            if tools.as_array().map(|a| !a.is_empty()).unwrap_or(false) {
                body.insert("tools".to_string(), tools);
                body.insert("tool_choice".to_string(), json!({"type": "auto"}));
            }
        }
        Ok(())
    }

    fn headers(&self) -> [HttpHeader<'_>; 2] {
        [
            HttpHeader {
                name: "x-api-key",
                value: self.context.api_key(),
            },
            HttpHeader {
                name: "anthropic-version",
                value: ANTHROPIC_VERSION,
            },
        ]
    }
}

impl BackendImpl for Anthropic {
    /// `anthropic_chat`
    fn chat<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(request)?;
        let url = self.context.endpoint_url(profile);
        let headers = self.headers();

        let http_request = self
            .context
            .json_request(&url, &post_data, HttpAuth::None, &headers);
        let response = post_json(http, &http_request, abort)?;
        Ok(parse_chat_response(&response.body)?)
    }

    fn chat_json<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        _schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        if !profile.supports_json_schema() {
            let fallback = ChatJsonPromptFallback::new(request, schema);
            let chat_req = fallback.chat_request();
            return self.chat(http, profile, &chat_req, abort);
        }

        let post_data = self.build_chat_json_body(request, schema)?;
        let url = self.context.endpoint_url(profile);
        let headers = self.headers();

        let http_request = self
            .context
            .json_request(&url, &post_data, HttpAuth::None, &headers);
        let response = post_json(http, &http_request, abort)?;
        Ok(parse_chat_response(&response.body)?)
    }

    /// `anthropic_infer_media`
    fn infer_media<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError> {
        if !profile.supports_vision() {
            return Err(InferMediaError::VisionUnsupported);
        }
        let user_prompt = request.user_prompt.unwrap_or("");
        if user_prompt.is_empty() {
            return Err(InferMediaError::IncompleteRequest);
        }
        let asset = single_media_asset(request.media)?;

        let prepared = prepare_asset(asset, profile, self.context.image_max_bytes())?;
        if !prepared.is_data_url() {
            return Err(InferMediaError::RequiresLocalImage);
        }
        let (mime, base64_data) =
            parse_data_url(prepared.payload()).ok_or(InferMediaError::PayloadPrepFailed)?;

        let mut body = Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert("max_tokens".to_string(), json!(self.context.max_tokens()));
        let system = request.system_prompt.unwrap_or("");
        if !system.is_empty() {
            body.insert("system".to_string(), json!(system));
        }
        body.insert(
            "messages".to_string(),
            json!([{
                "role": "user",
                "content": [
                    {"type": "text", "text": user_prompt},
                    {"type": "image", "source": {"type": "base64", "media_type": mime, "data": base64_data}}
                ]
            }]),
        );
        let body = Value::Object(body);
        let post_data = serde_json::to_string(&body)
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = self.context.endpoint_url(profile);
        let headers = self.headers();

        let http_request = self
            .context
            .json_request(&url, &post_data, HttpAuth::None, &headers);
        let response = post_json(http, &http_request, abort)?;

        let parsed = parse_chat_response(&response.body)?;
        match parsed.text {
            Some(t) if !t.is_empty() => Ok(t),
            _ => Err(ClawApiError::EmptyResponse.into()),
        }
    }

    async fn chat_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(request)?;
        let url = self.context.endpoint_url(profile);
        let headers = self.headers();

        let http_request = self
            .context
            .json_request(&url, &post_data, HttpAuth::None, &headers);
        let response = post_json_async(http, &http_request, cancel).await?;
        Ok(parse_chat_response(&response.body)?)
    }

    async fn chat_json_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        _schema_name: &str,
        schema: &Value,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        if !profile.supports_json_schema() {
            let fallback = ChatJsonPromptFallback::new(request, schema);
            let chat_req = fallback.chat_request();
            return self.chat_async(http, profile, &chat_req, cancel).await;
        }

        let post_data = self.build_chat_json_body(request, schema)?;
        let url = self.context.endpoint_url(profile);
        let headers = self.headers();

        let http_request = self
            .context
            .json_request(&url, &post_data, HttpAuth::None, &headers);
        let response = post_json_async(http, &http_request, cancel).await?;
        Ok(parse_chat_response(&response.body)?)
    }

    async fn infer_media_async<H: ClawHttpAsync>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<String, InferMediaError> {
        if !profile.supports_vision() {
            return Err(InferMediaError::VisionUnsupported);
        }
        let user_prompt = request.user_prompt.unwrap_or("");
        if user_prompt.is_empty() {
            return Err(InferMediaError::IncompleteRequest);
        }
        let asset = single_media_asset(request.media)?;

        let prepared = prepare_asset(asset, profile, self.context.image_max_bytes())?;
        if !prepared.is_data_url() {
            return Err(InferMediaError::RequiresLocalImage);
        }
        let (mime, base64_data) =
            parse_data_url(prepared.payload()).ok_or(InferMediaError::PayloadPrepFailed)?;

        let mut body = Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert("max_tokens".to_string(), json!(self.context.max_tokens()));
        let system = request.system_prompt.unwrap_or("");
        if !system.is_empty() {
            body.insert("system".to_string(), json!(system));
        }
        body.insert(
            "messages".to_string(),
            json!([{
                "role": "user",
                "content": [
                    {"type": "text", "text": user_prompt},
                    {"type": "image", "source": {"type": "base64", "media_type": mime, "data": base64_data}}
                ]
            }]),
        );
        let body = Value::Object(body);
        let post_data = serde_json::to_string(&body)
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = self.context.endpoint_url(profile);
        let headers = self.headers();

        let http_request = self
            .context
            .json_request(&url, &post_data, HttpAuth::None, &headers);
        let response = post_json_async(http, &http_request, cancel).await?;

        let parsed = parse_chat_response(&response.body)?;
        match parsed.text {
            Some(t) if !t.is_empty() => Ok(t),
            _ => Err(ClawApiError::EmptyResponse.into()),
        }
    }
}
