//! OpenAI-compatible backend, port of `claw_llm_backend_openai_compatible.c`.

use core::sync::atomic::AtomicBool;

use serde_json::{json, Value};

use claw_interface::http::{blocking::ClawHttp as BlockingClawHttp, Cancel, ClawHttp, HttpAuth};

use super::super::errors::{ChatError, ClawApiError, InferMediaError, InitError};
use super::super::media::prepare_asset;
use super::super::types::{ChatJsonRequest, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest};
use super::shared::{
    insert_tools_into_body, parse_openai_chat_response, post_json, post_json_async,
    single_media_asset, BackendContext,
};
use super::BackendImpl;

/// Chat endpoint path appended to the base URL.
const CHAT_PATH: &str = "/chat/completions";
/// Provider field name that carries the max-tokens value.
const MAX_TOKENS_FIELD: &str = "max_tokens";
/// Whether media prep must reject local/inline images (remote URLs only).
const IMAGE_REMOTE_URL_ONLY: bool = false;

pub(super) struct OpenAiCompatible {
    context: BackendContext,
}

impl OpenAiCompatible {
    /// `build_chat_body`
    fn build_chat_body(&self, request: &ChatRequest) -> Result<String, ChatError> {
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
            MAX_TOKENS_FIELD.to_string(),
            json!(self.context.max_tokens()),
        );
        body.insert("messages".to_string(), Value::Array(messages));

        if let Some(tools_json) = request.tools_json.filter(|s| !s.is_empty()) {
            insert_tools_into_body(&mut body, tools_json)?;
        }

        serde_json::to_string(&Value::Object(body)).map_err(|_| {
            ChatError::Api(ClawApiError::ApiError("out of memory serializing request"))
        })
    }

    fn build_chat_json_body(
        &self,
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
            MAX_TOKENS_FIELD.to_string(),
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
            insert_tools_into_body(&mut body, tools_json)?;
        }

        serde_json::to_string(&Value::Object(body)).map_err(|_| {
            ChatError::Api(ClawApiError::ApiError("out of memory serializing request"))
        })
    }
}

impl BackendImpl for OpenAiCompatible {
    /// `openai_compatible_init`
    fn make(config: &ClawApiConfig) -> Result<Self, InitError> {
        Ok(OpenAiCompatible {
            context: BackendContext::from_config(config),
        })
    }

    /// `openai_compatible_chat`
    fn chat<H: BlockingClawHttp>(
        &self,
        http: &mut H,
        request: &ChatRequest,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(request)?;
        let url = self.context.endpoint_url(CHAT_PATH);

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
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_json_body(request, schema_name, schema)?;
        let url = self.context.endpoint_url(CHAT_PATH);
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
        request: &MediaRequest,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError> {
        let Some(user_prompt) = request.user_prompt.filter(|prompt| !prompt.is_empty()) else {
            return Err(InferMediaError::IncompleteRequest);
        };
        let asset = single_media_asset(request.media)?;

        let prepared = prepare_asset(asset, IMAGE_REMOTE_URL_ONLY, self.context.image_max_bytes())?;

        let mut body = serde_json::Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert(
            MAX_TOKENS_FIELD.to_string(),
            json!(self.context.max_tokens()),
        );
        let mut messages: Vec<Value> = Vec::new();
        if let Some(system) = request.system_prompt.filter(|prompt| !prompt.is_empty()) {
            messages.push(json!({"role": "system", "content": system}));
        }
        messages.push(json!({"role": "user", "content": [
            {"type": "text", "text": user_prompt},
                {"type": "image_url", "image_url": {"url": prepared.payload()}}
        ]}));
        body.insert("messages".to_string(), Value::Array(messages));

        let post_data = serde_json::to_string(&Value::Object(body))
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = self.context.endpoint_url(CHAT_PATH);

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
        request: &ChatRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(request)?;
        let url = self.context.endpoint_url(CHAT_PATH);

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
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        cancel: Cancel<'_>,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_json_body(request, schema_name, schema)?;
        let url = self.context.endpoint_url(CHAT_PATH);
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
        request: &MediaRequest<'_>,
        cancel: Cancel<'_>,
    ) -> Result<String, InferMediaError> {
        let Some(user_prompt) = request.user_prompt.filter(|prompt| !prompt.is_empty()) else {
            return Err(InferMediaError::IncompleteRequest);
        };
        let asset = single_media_asset(request.media)?;

        let prepared = prepare_asset(asset, IMAGE_REMOTE_URL_ONLY, self.context.image_max_bytes())?;

        let mut body = serde_json::Map::new();
        body.insert("model".to_string(), json!(self.context.model()));
        body.insert(
            MAX_TOKENS_FIELD.to_string(),
            json!(self.context.max_tokens()),
        );
        let mut messages: Vec<Value> = Vec::new();
        if let Some(system) = request.system_prompt.filter(|prompt| !prompt.is_empty()) {
            messages.push(json!({"role": "system", "content": system}));
        }
        messages.push(json!({"role": "user", "content": [
            {"type": "text", "text": user_prompt},
                {"type": "image_url", "image_url": {"url": prepared.payload()}}
        ]}));
        body.insert("messages".to_string(), Value::Array(messages));

        let post_data = serde_json::to_string(&Value::Object(body))
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = self.context.endpoint_url(CHAT_PATH);

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
