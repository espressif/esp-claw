#![allow(dead_code)]

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};

use claw_agent::{AgentPersistenceConfig, AgentSystem, SessionEvent, SessionEventStream};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::http::{
    Cancel, ClawHttp, HttpError, HttpJsonRequest, HttpResponseFuture, HttpStatusCode, SliceChunks,
    StreamingHttp,
};
use claw_interface::{
    BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp, StdThread, TokioExecutor,
};
use futures_lite::StreamExt;
use serde_json::{json, Value};

pub type MemAgentSystem =
    AgentSystem<MemFs, Sse<BlockingHttpAdapter<SharedScriptHttp>>, ImmediateTimer>;

/// Wraps any [`ClawHttp`] test double so it can back the streaming iteration
/// loop: the one-shot seam returns the scripted OpenAI JSON verbatim (for the
/// memory adapters' `chat`), while the streaming seam converts that same JSON
/// into a single-shot SSE body (for the iteration loop's `chat_stream`). This
/// keeps every existing non-streaming fixture usable unchanged, and lives only
/// in the test harness — the shared `claw-interface` stays format-agnostic.
#[derive(Default)]
pub struct Sse<T>(pub T);

impl<T: ClawHttp> ClawHttp for Sse<T> {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        self.0.post_json(request, cancel)
    }
}

impl<T: ClawHttp> StreamingHttp for Sse<T> {
    type ByteStream = SliceChunks;

    fn post_json_streaming<'a, 'r>(
        &'a mut self,
        request: &'r HttpJsonRequest<'r>,
        cancel: Cancel<'a>,
    ) -> impl std::future::Future<Output = Result<(HttpStatusCode, SliceChunks), HttpError>> {
        async move {
            let response = self.0.post_json(request, cancel).await?;
            let sse = openai_json_to_sse(&response.body);
            Ok((response.status_code, SliceChunks::once(sse.into_bytes())))
        }
    }
}

/// Convert a scripted OpenAI `chat/completions` JSON response into an equivalent
/// SSE body: reasoning, then content, then one tool-call delta each, then
/// `[DONE]`. The `OpenAiSse` parser reconstructs the same `LlmResponse`.
fn openai_json_to_sse(body: &str) -> String {
    let Ok(root) = serde_json::from_str::<Value>(body) else {
        return "data: [DONE]\n\n".to_string();
    };
    let message = root
        .get("choices")
        .and_then(|choices| choices.get(0))
        .and_then(|choice| choice.get("message"));
    let Some(message) = message else {
        return "data: [DONE]\n\n".to_string();
    };

    let mut out = String::new();
    let mut frame = |delta: Value| {
        out.push_str("data: ");
        out.push_str(&json!({ "choices": [{ "delta": delta }] }).to_string());
        out.push_str("\n\n");
    };
    if let Some(reasoning) = message
        .get("reasoning_content")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
    {
        frame(json!({ "reasoning_content": reasoning }));
    }
    if let Some(content) = message
        .get("content")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
    {
        frame(json!({ "content": content }));
    }
    if let Some(tool_calls) = message.get("tool_calls").and_then(Value::as_array) {
        for (index, call) in tool_calls.iter().enumerate() {
            let function = call.get("function");
            frame(json!({
                "tool_calls": [{
                    "index": index,
                    "id": call.get("id"),
                    "function": {
                        "name": function.and_then(|f| f.get("name")),
                        "arguments": function.and_then(|f| f.get("arguments")),
                    },
                }],
            }));
        }
    }
    if let Some(usage) = root.get("usage") {
        out.push_str("data: ");
        out.push_str(&json!({ "choices": [], "usage": usage }).to_string());
        out.push_str("\n\n");
    }
    out.push_str("data: [DONE]\n\n");
    out
}

static MEM_ROOT_ID: AtomicU64 = AtomicU64::new(1);

pub fn serialize_script() -> std::sync::MutexGuard<'static, ()> {
    SharedScriptHttp::serialize()
}

pub fn mem_root(name: &str) -> String {
    let id = MEM_ROOT_ID.fetch_add(1, Ordering::Relaxed);
    format!("/{name}-{id}")
}

pub fn build_mem_system(root: &str, bodies: Vec<String>) -> MemAgentSystem {
    install_script(bodies);
    let system = MemAgentSystem::new::<StdThread, TokioExecutor>(persistence(root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    system
}

pub fn assistant_text(text: &str) -> String {
    json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
}

pub fn drain_until_turn_ended(events: &mut SessionEventStream) -> Vec<SessionEvent> {
    futures_lite::future::block_on(async move {
        let mut collected = Vec::new();
        while let Some(event) = events.next().await {
            let ended = matches!(event, SessionEvent::TurnEnded { .. });
            collected.push(event);
            if ended {
                break;
            }
        }
        collected
    })
}

pub fn install_script(bodies: Vec<String>) {
    let mut script = Vec::with_capacity(bodies.len().saturating_add(1));
    if !bodies.is_empty() {
        script.push(assistant_text("[]"));
    }
    script.extend(bodies);
    SharedScriptHttp::install(script);
}

pub fn persistence(root: &str) -> AgentPersistenceConfig {
    AgentPersistenceConfig {
        persistence_root: root.to_string(),
        skill_roots: Vec::new(),
    }
}

pub fn llm_config() -> ClawApiConfig {
    ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-test",
        "gpt-test",
        "https://example.invalid",
    )
}

pub fn csv_dicts(input: &str) -> Vec<BTreeMap<String, String>> {
    let mut records = csv_records(input);
    assert!(!records.is_empty(), "csv fixture must include a header row");
    let headers = records.remove(0);
    records
        .into_iter()
        .enumerate()
        .map(|(index, record)| {
            assert_eq!(
                record.len(),
                headers.len(),
                "csv row {} has {} fields, expected {}",
                index + 2,
                record.len(),
                headers.len()
            );
            headers
                .iter()
                .cloned()
                .zip(record)
                .collect::<BTreeMap<_, _>>()
        })
        .collect()
}

fn csv_records(input: &str) -> Vec<Vec<String>> {
    let mut records = Vec::new();
    let mut record = Vec::new();
    let mut field = String::new();
    let mut chars = input.chars().peekable();
    let mut in_quotes = false;
    let mut field_started = false;

    while let Some(ch) = chars.next() {
        if in_quotes {
            match ch {
                '"' if chars.peek() == Some(&'"') => {
                    chars.next();
                    field.push('"');
                }
                '"' => in_quotes = false,
                _ => field.push(ch),
            }
            field_started = true;
            continue;
        }

        match ch {
            '"' if !field_started => {
                in_quotes = true;
                field_started = true;
            }
            ',' => {
                record.push(std::mem::take(&mut field));
                field_started = false;
            }
            '\n' => {
                record.push(std::mem::take(&mut field));
                records.push(std::mem::take(&mut record));
                field_started = false;
            }
            '\r' => {
                if chars.peek() == Some(&'\n') {
                    chars.next();
                }
                record.push(std::mem::take(&mut field));
                records.push(std::mem::take(&mut record));
                field_started = false;
            }
            _ => {
                field.push(ch);
                field_started = true;
            }
        }
    }

    assert!(!in_quotes, "csv fixture has an unterminated quoted field");
    if field_started || !field.is_empty() || !record.is_empty() {
        record.push(field);
        records.push(record);
    }
    records
}
