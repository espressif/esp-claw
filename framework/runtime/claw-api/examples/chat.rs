//! Build a [`ClawApi`] over a stub transport and issue a plain chat and a
//! structured-JSON chat.
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

use claw_api::{BackendKind, ChatJsonRequest, ChatRequest, ClawApi, ClawApiConfig};
use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode};
use serde::Deserialize;
use serde_json::json;

/// A canned transport: inspects the outgoing body to decide which reply to
/// return — a structured object when the request asks for `response_format`,
/// otherwise a plain greeting.
struct StubHttp;

impl ClawHttp for StubHttp {
    fn post_json(
        &mut self,
        request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        let body = if request.body.contains("response_format") {
            r#"{"choices":[{"message":{"role":"assistant","content":"{\"city\":\"Shanghai\",\"temp_c\":21}"}}]}"#
        } else {
            r#"{"choices":[{"message":{"role":"assistant","content":"Hello!"}}]}"#
        };
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: body.to_string(),
        })
    }
}

/// The structured shape we ask the model to fill.
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

fn main() -> anyhow::Result<()> {
    let config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-demo",
        "gpt-4o-mini",
        "https://api.example.com/v1",
    );
    let mut api = ClawApi::init(config, StubHttp)?;
    let abort = AtomicBool::new(false);

    // 1. Plain chat → free-form text (+ any tool calls).
    let messages = json!([{ "role": "user", "content": "Say hello." }]);
    let reply = api.chat(&ChatRequest::new("You are concise.", &messages), &abort)?;
    println!("chat       -> {:?}", reply.text);

    // 2. Structured chat → a typed `Weather`, parsed and validated for you.
    let messages = json!([{ "role": "user", "content": "Weather in Shanghai?" }]);
    let out = api.chat_json::<Weather>(
        &ChatJsonRequest::new("You are a weather service.", &messages)
            .with_output_schema("weather", WEATHER_SCHEMA),
        &abort,
    )?;
    match out.output {
        Some(Weather { city, temp_c }) => println!("chat_json  -> {city}: {temp_c}C"),
        None => println!("chat_json  -> (model returned tool calls, no object)"),
    }

    Ok(())
}
