//! Integration tests for [`LlmCompactor`].
//!
//! A canned `ClawHttp` stands in for the network so the test is hermetic.

use std::sync::atomic::AtomicBool;

use claw_api::{ClawApi, ClawApiConfig};
use claw_core::LlmCompactor;
use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
use claw_memory::Compactor;
use serde_json::json;

/// Returns a fixed OpenAI-shaped reply with `content` set to `reply`.
struct CannedHttp {
    body: String,
}

impl ClawHttp for CannedHttp {
    fn post_json(
        &mut self,
        _request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        Ok(HttpResponse {
            status_code: 200,
            body: self.body.clone(),
        })
    }
}

fn api_replying(reply: &str) -> ClawApi<CannedHttp> {
    let body = json!({
        "choices": [{ "message": { "role": "assistant", "content": reply } }]
    })
    .to_string();
    let http = CannedHttp { body };
    ClawApi::init(
        ClawApiConfig {
            api_key: Some("key".into()),
            backend_type: "openai_compatible".into(),
            model: Some("model-x".into()),
            base_url: Some("https://api.example.com/v1".into()),
            ..Default::default()
        },
        http,
    )
    .expect("init ClawApi")
}

#[test]
fn summarizes_window_into_one_system_message() {
    let compactor = LlmCompactor::new(api_replying("short recap"));
    let window = vec![
        json!({ "role": "user", "content": "hello" }),
        json!({ "role": "assistant", "content": "hi" }),
        json!({ "role": "user", "content": "remember the budget is $50" }),
    ];

    let out = compactor.compact(&window).expect("compaction succeeds");

    assert_eq!(out.len(), 1);
    assert_eq!(out[0]["role"], "system");
    let content = out[0]["content"]
        .as_str()
        .expect("summary content is a string");
    assert!(content.contains("short recap"), "got: {content}");
}

#[test]
fn empty_summary_is_an_error() {
    let compactor = LlmCompactor::new(api_replying(""));
    let window = vec![json!({ "role": "user", "content": "x" })];

    assert!(compactor.compact(&window).is_err());
}
