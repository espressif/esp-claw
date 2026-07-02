//! OpenAI-compatible backend, port of `claw_llm_backend_openai_compatible.c`.

use core::sync::atomic::AtomicBool;

use serde_json::{json, Value};

use claw_interface::http::{blocking::ClawHttp as BlockingClawHttp, Cancel, ClawHttp, HttpAuth};

use super::super::errors::{ChatError, ClawApiError, InferMediaError, InitError};
use super::super::media::prepare_asset;
use super::super::types::{
    ChatJsonRequest, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest, ModelProfile,
};
use super::shared::{
    insert_tools_into_body, parse_openai_chat_response, post_json, post_json_async,
    single_media_asset, BackendContext, ChatJsonPromptFallback,
};
use super::BackendImpl;

pub(super) const ID: &str = "openai_compatible";
pub(super) const CHAT_PATH: &str = "/chat/completions";
pub(super) const DEFAULT_MAX_TOKENS_FIELD: &str = "max_tokens";

pub(super) struct OpenAiCompatible {
    context: BackendContext,
}

/// `openai_compatible_init`
///
/// Credential/config validation is centralized in [`crate::ClawApi::init`];
/// `api_key`, `model`, and `base_url` are guaranteed non-empty here.
impl OpenAiCompatible {
    pub(super) fn make(config: &ClawApiConfig) -> Result<Self, InitError> {
        Ok(OpenAiCompatible {
            context: BackendContext::from_config(config),
        })
    }

    /// `build_chat_body`
    fn build_chat_body(
        &self,
        profile: &ModelProfile,
        request: &ChatRequest,
    ) -> Result<String, ChatError> {
        let mut messages: Vec<Value> = Vec::new();
        if !request.system_prompt.is_empty() {
            messages.push(json!({"role": "system", "content": request.system_prompt}));
        }
        if let Some(arr) = request.messages.as_array() {
            messages.extend(arr.iter().cloned());
        }
        // Ephemeral trailing reminders, appended after the persisted history.
        messages.extend(request.reminders.iter().cloned());

        let mut body = serde_json::Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert(
            profile.max_tokens_field().to_string(),
            json!(self.context.max_tokens()),
        );
        body.insert("messages".to_string(), Value::Array(messages));

        if let Some(tools_json) = request.tools_json.filter(|s| !s.is_empty()) {
            insert_tools_into_body(&mut body, profile, tools_json)?;
        }

        serde_json::to_string(&Value::Object(body)).map_err(|_| {
            ChatError::Api(ClawApiError::ApiError("out of memory serializing request"))
        })
    }

    fn build_chat_json_body(
        &self,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
    ) -> Result<String, ChatError> {
        let mut messages: Vec<Value> = Vec::new();
        if !request.system_prompt.is_empty() {
            messages.push(json!({"role": "system", "content": request.system_prompt}));
        }
        if let Some(arr) = request.messages.as_array() {
            messages.extend(arr.iter().cloned());
        }
        // Ephemeral trailing reminders, appended after the persisted history.
        messages.extend(request.reminders.iter().cloned());

        let mut body = serde_json::Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert(
            profile.max_tokens_field().to_string(),
            json!(self.context.max_tokens()),
        );
        body.insert("messages".to_string(), Value::Array(messages));
        body.insert(
            "response_format".to_string(),
            json!({
                "type": "json_schema",
                "json_schema": {
                    "name": schema_name,
                    "strict": true,
                    "schema": schema,
                }
            }),
        );
        if let Some(tools_json) = request.tools_json.filter(|s| !s.is_empty()) {
            insert_tools_into_body(&mut body, profile, tools_json)?;
        }

        serde_json::to_string(&Value::Object(body)).map_err(|_| {
            ChatError::Api(ClawApiError::ApiError("out of memory serializing request"))
        })
    }
}

impl BackendImpl for OpenAiCompatible {
    /// `openai_compatible_chat`
    fn chat<H: BlockingClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(profile, request)?;
        let url = self.context.endpoint_url(profile);

        let http_request = self.context.json_request(
            &url,
            &post_data,
            HttpAuth::Bearer(self.context.api_key()),
            &[],
        );
        let response = post_json(http, &http_request, abort)?;
        Ok(parse_openai_chat_response(&response.body)?)
    }

    fn chat_json<H: BlockingClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        if !profile.supports_json_schema() {
            let fallback = ChatJsonPromptFallback::new(request, schema);
            let chat_req = fallback.chat_request();
            return self.chat(http, profile, &chat_req, abort);
        }

        let post_data = self.build_chat_json_body(profile, request, schema_name, schema)?;
        let url = self.context.endpoint_url(profile);
        let http_request = self.context.json_request(
            &url,
            &post_data,
            HttpAuth::Bearer(self.context.api_key()),
            &[],
        );
        let response = post_json(http, &http_request, abort)?;
        parse_openai_chat_response(&response.body).map_err(ChatError::from)
    }

    /// `openai_compatible_infer_media`
    fn infer_media<H: BlockingClawHttp>(
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

        let mut body = serde_json::Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert(
            profile.max_tokens_field().to_string(),
            json!(self.context.max_tokens()),
        );
        let mut messages: Vec<Value> = Vec::new();
        let system = request.system_prompt.unwrap_or("");
        if !system.is_empty() {
            messages.push(json!({"role": "system", "content": system}));
        }
        messages.push(json!({"role": "user", "content": [
            {"type": "text", "text": user_prompt},
                {"type": "image_url", "image_url": {"url": prepared.payload()}}
        ]}));
        body.insert("messages".to_string(), Value::Array(messages));

        let post_data = serde_json::to_string(&Value::Object(body))
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = self.context.endpoint_url(profile);

        let http_request = self.context.json_request(
            &url,
            &post_data,
            HttpAuth::Bearer(self.context.api_key()),
            &[],
        );
        let response = post_json(http, &http_request, abort)?;

        let parsed = parse_openai_chat_response(&response.body)?;
        match parsed.text {
            Some(t) if !t.is_empty() => Ok(t),
            _ => Err(ClawApiError::EmptyResponse.into()),
        }
    }

    async fn chat_async<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(profile, request)?;
        let url = self.context.endpoint_url(profile);

        let http_request = self.context.json_request(
            &url,
            &post_data,
            HttpAuth::Bearer(self.context.api_key()),
            &[],
        );
        let response = post_json_async(http, &http_request, cancel).await?;
        Ok(parse_openai_chat_response(&response.body)?)
    }

    async fn chat_json_async<H: ClawHttp>(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        if !profile.supports_json_schema() {
            let fallback = ChatJsonPromptFallback::new(request, schema);
            let chat_req = fallback.chat_request();
            return self.chat_async(http, profile, &chat_req, cancel).await;
        }

        let post_data = self.build_chat_json_body(profile, request, schema_name, schema)?;
        let url = self.context.endpoint_url(profile);
        let http_request = self.context.json_request(
            &url,
            &post_data,
            HttpAuth::Bearer(self.context.api_key()),
            &[],
        );
        let response = post_json_async(http, &http_request, cancel).await?;
        parse_openai_chat_response(&response.body).map_err(ChatError::from)
    }

    async fn infer_media_async<H: ClawHttp>(
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

        let mut body = serde_json::Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert(
            profile.max_tokens_field().to_string(),
            json!(self.context.max_tokens()),
        );
        let mut messages: Vec<Value> = Vec::new();
        let system = request.system_prompt.unwrap_or("");
        if !system.is_empty() {
            messages.push(json!({"role": "system", "content": system}));
        }
        messages.push(json!({"role": "user", "content": [
            {"type": "text", "text": user_prompt},
                {"type": "image_url", "image_url": {"url": prepared.payload()}}
        ]}));
        body.insert("messages".to_string(), Value::Array(messages));

        let post_data = serde_json::to_string(&Value::Object(body))
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = self.context.endpoint_url(profile);

        let http_request = self.context.json_request(
            &url,
            &post_data,
            HttpAuth::Bearer(self.context.api_key()),
            &[],
        );
        let response = post_json_async(http, &http_request, cancel).await?;

        let parsed = parse_openai_chat_response(&response.body)?;
        match parsed.text {
            Some(t) if !t.is_empty() => Ok(t),
            _ => Err(ClawApiError::EmptyResponse.into()),
        }
    }
}
