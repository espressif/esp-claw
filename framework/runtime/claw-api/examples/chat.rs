//! Blocking [`ClawApi`] surface: plain chat, tool-calling chat, and structured
//! JSON chat — exercising every [`ChatRequest`] / [`ChatJsonRequest`] builder
//! and reading back every [`LlmResponse`] / [`ChatJsonResponse`] field.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-api --example chat --target x86_64-unknown-linux-gnu
//! ```
//!
//! Networking is injected: `claw-api` never opens sockets. Here the transport
//! returns canned OpenAI-shaped replies so the example is self-contained; on
//! device the espidf layer implements [`ClawHttp`] over `esp_http_client`.

use std::sync::atomic::AtomicBool;

use claw_api::{
    BackendKind, ChatJsonRequest, ChatRequest, ClawApi, ClawApiConfig, RetryPolicy, ToolCall,
};
use claw_interface::http::{
    blocking::ClawHttp, HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode,
};
use serde::Deserialize;
use serde_json::json;

/// A canned transport that inspects the outgoing body to decide which reply to
/// return: a structured object for a `response_format` request, a tool call
/// when tools were offered, otherwise a plain greeting.
struct StubHttp;

impl ClawHttp for StubHttp {
    fn post_json(
        &mut self,
        request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        let body = if request.body.contains("response_format") {
            r#"{"choices":[{"message":{"role":"assistant",
                "content":"{\"city\":\"Shanghai\",\"temp_c\":21}"}}]}"#
        } else if request.body.contains("tools") {
            // An assistant turn that emits a tool call plus reasoning text.
            r#"{"choices":[{"message":{"role":"assistant","content":null,
                "reasoning_content":"The user wants weather; call the tool.",
                "tool_calls":[{"id":"call_1","type":"function",
                    "function":{"name":"get_weather","arguments":"{\"city\":\"Shanghai\"}"}}]}}]}"#
        } else {
            r#"{"choices":[{"message":{"role":"assistant","content":"Hello!"}}]}"#
        };
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: body.to_string(),
        })
    }
}

/// The structured shape we ask the model to fill in the `chat_json` call.
#[derive(Debug, Deserialize)]
struct Weather {
    city: String,
    temp_c: i32,
}

const WEATHER_SCHEMA: &str = r#"{
    "type": "object",
    "properties": {
        "city":   { "type": "string" },
        "temp_c": { "type": "integer" }
    },
    "required": ["city", "temp_c"],
    "additionalProperties": false
}"#;

const WEATHER_TOOL: &str = r#"[{
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "Look up the weather for a city.",
        "parameters": {
            "type": "object",
            "properties": { "city": { "type": "string" } },
            "required": ["city"]
        }
    }
}]"#;

fn main() -> anyhow::Result<()> {
    // `BackendKind` selects the wire format; both built-ins are shown here.
    let backends = [
        BackendKind::OpenAiCompatible,
        BackendKind::AnthropicCompatible,
    ];
    println!("backends   -> {:?}", backends.map(BackendKind::as_str));

    let config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-demo",
        "gpt-4o-mini",
        "https://api.example.com/v1",
    );
    let mut api = ClawApi::new(StubHttp);
    api.set_config(config)?;
    let abort = AtomicBool::new(false);

    // 1. Plain chat → free-form assistant text.
    let messages = json!([{ "role": "user", "content": "Say hello." }]);
    let request = ChatRequest::new("You are concise.", &messages);
    let reply = api.chat(&request, &abort)?;
    println!("chat.text  -> {:?}", reply.text);
    println!("chat.calls -> {}", reply.tool_calls.len());

    // 2. Tool-calling chat: attach tools, an ephemeral reminder, and a custom
    //    retry policy — then read back tool calls, reasoning, and raw JSON.
    let reminders = json!([{ "role": "system", "content": "Prefer metric units." }]);
    let reminders = reminders.as_array().expect("reminders array");
    let request = ChatRequest::new("You are a weather agent.", &messages)
        .with_tools(WEATHER_TOOL)
        .with_reminders(reminders)
        .with_retry(RetryPolicy::fixed(2, 100));
    let reply = api.chat(&request, &abort)?;
    for ToolCall {
        id,
        name,
        arguments_json,
    } in &reply.tool_calls
    {
        println!("tool_call  -> {id} {name}({arguments_json})");
    }
    println!("reasoning  -> {:?}", reply.reasoning_content);
    println!("raw json?  -> {}", reply.raw_message_json.is_some());

    // 3. Structured chat → a typed `Weather`, parsed and validated for you.
    let messages = json!([{ "role": "user", "content": "Weather in Shanghai?" }]);
    let json_request = ChatJsonRequest::new("You are a weather service.", &messages)
        .with_output_schema("weather", WEATHER_SCHEMA)
        .with_tools(WEATHER_TOOL)
        .with_reminders(reminders)
        .with_retry(RetryPolicy::none());
    // The attached schema is readable back through the public field.
    if let Some(schema) = json_request.output_schema {
        println!(
            "schema     -> {} = {} bytes",
            schema.name,
            schema.json.len()
        );
    }
    let out = api.chat_json::<Weather>(&json_request, &abort)?;
    match out.output {
        Some(Weather { city, temp_c }) => println!("chat_json  -> {city}: {temp_c}C"),
        None => println!("chat_json  -> (model returned tool calls, no object)"),
    }
    println!(
        "json.extra -> calls={} reasoning={:?} raw?={}",
        out.tool_calls.len(),
        out.reasoning_content,
        out.raw_message_json.is_some()
    );

    Ok(())
}
