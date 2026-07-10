//! End-to-end streaming tests: drive [`ClawApiAsync::chat_stream`] over the
//! `ChunkedHttp` double, which serves an SSE body in small byte chunks so the
//! parser is exercised across arbitrary frame/codepoint splits.

use std::sync::atomic::AtomicBool;

use claw_api::{BackendKind, ChatRequest, ClawApiAsync, ClawApiConfig, LlmDelta};
use claw_interface::http::ChunkedHttp;
use claw_interface::{Cancel, ImmediateTimer};
use futures_lite::future::block_on;
use futures_lite::StreamExt;
use serde_json::json;

fn config(backend: BackendKind) -> ClawApiConfig {
    ClawApiConfig::new(backend, "key", "model", "http://example.test/v1")
}

/// Drive a `chat_stream` to completion, returning the deltas and final response.
fn run(
    backend: BackendKind,
    sse_body: &str,
    chunk_size: usize,
) -> (Vec<LlmDelta>, claw_api::LlmResponse) {
    let http = ChunkedHttp::new([sse_body.to_string()], chunk_size);
    let mut rt = ClawApiAsync::<ChunkedHttp, ImmediateTimer>::init(config(backend), http).unwrap();
    let abort = AtomicBool::new(false);
    let messages = json!([{ "role": "user", "content": "hi" }]);
    let request = ChatRequest::new("sys", &messages);

    block_on(async {
        let mut stream = rt.chat_stream(&request, Cancel::new(&abort)).await.unwrap();
        let mut deltas = Vec::new();
        while let Some(delta) = stream.next().await {
            deltas.push(delta.expect("stream delta"));
        }
        let response = stream
            .take_response()
            .expect("response after drain")
            .expect("assembled response");
        (deltas, response)
    })
}

#[test]
fn openai_streams_fragments_then_assembles_response() {
    let sse = concat!(
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"th\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"ink\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"ping\",\"arguments\":\"\"}}]}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{}\"}}]}}]}\n\n",
        "data: [DONE]\n\n",
    );
    // 7-byte chunks split SSE frames and JSON tokens mid-way on purpose.
    let (deltas, response) = run(BackendKind::OpenAiCompatible, sse, 7);

    assert_eq!(
        deltas,
        vec![
            LlmDelta::Reasoning("th".to_string()),
            LlmDelta::Reasoning("ink".to_string()),
            LlmDelta::Output("Hel".to_string()),
            LlmDelta::Output("lo".to_string()),
            LlmDelta::ToolCall {
                index: 0,
                id: "call_1".to_string(),
                name: "ping".to_string(),
                arguments: "{}".to_string(),
            },
        ]
    );
    assert_eq!(response.text.as_deref(), Some("Hello"));
    assert_eq!(response.reasoning_content.as_deref(), Some("think"));
    assert_eq!(response.tool_calls.len(), 1);
    assert_eq!(response.tool_calls[0].name, "ping");
    assert!(response.raw_message_json.is_some());
}

#[test]
fn anthropic_streams_fragments_then_assembles_response() {
    let sse = concat!(
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n",
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"ping\",\"input\":{}}}\n\n",
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n",
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n",
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
    );
    let (deltas, response) = run(BackendKind::AnthropicCompatible, sse, 9);

    assert_eq!(
        deltas,
        vec![
            LlmDelta::Output("Hi".to_string()),
            LlmDelta::ToolCall {
                index: 0,
                id: "toolu_1".to_string(),
                name: "ping".to_string(),
                arguments: "{}".to_string(),
            },
        ]
    );
    assert_eq!(response.text.as_deref(), Some("Hi"));
    assert_eq!(response.tool_calls[0].name, "ping");
}
