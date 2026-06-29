//! OpenAI-compatible backend, port of `claw_llm_backend_openai_compatible.c`.

use core::sync::atomic::AtomicBool;

use serde_json::{json, Value};

use claw_interface::http::{ClawHttp, HttpJsonRequest};

use super::super::backend::{BackendDefaults, BackendRegistration, LlmBackend};
use super::super::errors::{ChatError, ClawApiError, InferMediaError, InitError};
use super::super::media::prepare_asset;
use super::super::types::{
    ChatJsonRequest, ChatRequest, ClawApiConfig, LlmResponse, MediaRequest, ModelProfile,
};
use super::common::{
    chat_json_prompt_fallback, insert_tools_into_body, join_url, map_http_error,
    parse_openai_chat_response, single_media_asset,
};

pub const ID: &str = "openai_compatible";
pub const CHAT_PATH: &str = "/chat/completions";
pub const AUTH_TYPE: &str = "bearer";
pub const DEFAULT_MAX_TOKENS_FIELD: &str = "max_tokens";

pub fn registration<H: ClawHttp>() -> BackendRegistration<H> {
    BackendRegistration {
        defaults: BackendDefaults {
            auth_type: AUTH_TYPE,
            chat_path: CHAT_PATH,
            max_tokens_field: DEFAULT_MAX_TOKENS_FIELD,
        },
        make: make::<H>,
    }
}

struct OpenAiCompatible {
    api_key: String,
    model: String,
    base_url: String,
    auth_type: String,
    timeout_ms: u32,
    max_tokens: u32,
    image_max_bytes: usize,
}

/// `openai_compatible_init`
///
/// Credential/config validation is centralized in [`crate::ClawApi::init`];
/// `api_key`, `model`, and `base_url` are guaranteed non-empty here.
fn make<H: ClawHttp>(config: &ClawApiConfig) -> Result<Box<dyn LlmBackend<H>>, InitError> {
    let api_key = config.api_key.as_deref().unwrap_or("");
    let model = config.model.as_deref().unwrap_or("");
    let base_url = config.base_url.as_deref().unwrap_or("");
    let auth_type = match config.auth_type.as_deref() {
        Some(a) if !a.is_empty() => a,
        _ => "bearer",
    };

    Ok(Box::new(OpenAiCompatible {
        api_key: api_key.to_string(),
        model: model.to_string(),
        base_url: base_url.to_string(),
        auth_type: auth_type.to_string(),
        timeout_ms: config.timeout_ms,
        max_tokens: config.max_tokens,
        image_max_bytes: config.image_max_bytes,
    }))
}

impl OpenAiCompatible {
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
        body.insert("model".to_string(), json!(self.model));
        body.insert(profile.max_tokens_field.clone(), json!(self.max_tokens));
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
        body.insert("model".to_string(), json!(self.model));
        body.insert(profile.max_tokens_field.clone(), json!(self.max_tokens));
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

impl<H: ClawHttp> LlmBackend<H> for OpenAiCompatible {
    /// `openai_compatible_chat`
    fn chat(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatRequest,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        let post_data = self.build_chat_body(profile, request)?;
        let url = join_url(&self.base_url, &profile.chat_path);

        let http_request = HttpJsonRequest {
            url: &url,
            body: &post_data,
            api_key: Some(&self.api_key),
            auth_type: Some(&self.auth_type),
            timeout_ms: self.timeout_ms,
            headers: &[],
        };
        let response = http
            .post_json(&http_request, abort)
            .map_err(map_http_error)?;
        Ok(parse_openai_chat_response(&response.body)?)
    }

    fn chat_json(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &ChatJsonRequest<'_>,
        schema_name: &str,
        schema: &Value,
        abort: &AtomicBool,
    ) -> Result<LlmResponse, ChatError> {
        if !profile.supports_json_schema {
            return chat_json_prompt_fallback(self, http, profile, request, schema, abort);
        }

        let post_data = self.build_chat_json_body(profile, request, schema_name, schema)?;
        let url = join_url(&self.base_url, &profile.chat_path);
        let http_request = HttpJsonRequest {
            url: &url,
            body: &post_data,
            api_key: Some(&self.api_key),
            auth_type: Some(&self.auth_type),
            timeout_ms: self.timeout_ms,
            headers: &[],
        };
        let response = http
            .post_json(&http_request, abort)
            .map_err(map_http_error)?;
        parse_openai_chat_response(&response.body).map_err(ChatError::from)
    }

    /// `openai_compatible_infer_media`
    fn infer_media(
        &self,
        http: &mut H,
        profile: &ModelProfile,
        request: &MediaRequest,
        abort: &AtomicBool,
    ) -> Result<String, InferMediaError> {
        if !profile.supports_vision {
            return Err(InferMediaError::VisionUnsupported);
        }
        let user_prompt = request.user_prompt.unwrap_or("");
        if user_prompt.is_empty() {
            return Err(InferMediaError::IncompleteRequest);
        }
        let asset = single_media_asset(request.media)?;

        let prepared = prepare_asset(asset, profile, self.image_max_bytes)?;

        let body = json!({
            "model": self.model,
            // max_tokens field name is dynamic; insert separately below.
        });
        let mut body = body.as_object().unwrap().clone();
        body.insert(profile.max_tokens_field.clone(), json!(self.max_tokens));
        let mut messages: Vec<Value> = Vec::new();
        let system = request.system_prompt.unwrap_or("");
        if !system.is_empty() {
            messages.push(json!({"role": "system", "content": system}));
        }
        messages.push(json!({"role": "user", "content": [
            {"type": "text", "text": user_prompt},
            {"type": "image_url", "image_url": {"url": prepared.payload}}
        ]}));
        body.insert("messages".to_string(), Value::Array(messages));

        let post_data = serde_json::to_string(&Value::Object(body))
            .map_err(|_| ClawApiError::ApiError("out of memory serializing media request"))?;
        let url = join_url(&self.base_url, &profile.chat_path);

        let http_request = HttpJsonRequest {
            url: &url,
            body: &post_data,
            api_key: Some(&self.api_key),
            auth_type: Some(&self.auth_type),
            timeout_ms: self.timeout_ms,
            headers: &[],
        };
        let response = http
            .post_json(&http_request, abort)
            .map_err(map_http_error)?;

        let parsed = parse_openai_chat_response(&response.body)?;
        match parsed.text {
            Some(t) if !t.is_empty() => Ok(t),
            _ => Err(ClawApiError::EmptyResponse.into()),
        }
    }
}
