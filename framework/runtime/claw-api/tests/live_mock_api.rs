//! Integration tests against a publicly hosted **mock** LLM endpoint.
//!
//! Unlike the in-process `MockHttp` unit tests in `lib.rs` (which assert the
//! request/response wire format with no network), these exercise the *real*
//! transport + parse round-trip over HTTPS using a free, keyless mock service:
//! [MockAI](https://mock-ai.fly.dev). MockAI needs no API key and echoes the
//! last user message back as the assistant reply, so we can assert the full
//! path end-to-end without a real provider or secret.
//!
//! They are `#[ignore]`d because they hit a third-party service over the
//! network (which can rate-limit, change, or go down). Run them explicitly:
//!
//! ```text
//! cargo test -p claw-api --test live_mock_api --target x86_64-unknown-linux-gnu -- --ignored
//! ```
//!
//! ## Provider coverage
//!
//! DeepSeek, Qwen, MiniMax, Kimi (Moonshot), GLM (Zhipu), and OpenAI itself are
//! all OpenAI-**compatible**: they share the exact wire format of the
//! `openai_compatible` backend and differ only by `base_url`/`model`. So the
//! per-provider cases below run that one backend with each provider's config
//! (the real base URLs are recorded for reference), pointed at the mock.
//! Anthropic/Claude uses the separate `anthropic_compatible` backend
//! (`/messages`).

use std::sync::atomic::AtomicBool;

use claw_api::{BackendKind, ChatJsonRequest, ChatRequest, ClawApi, ClawApiConfig};
use claw_interface::http::blocking::RealHttp;
use serde_json::json;

/// Free, keyless mock LLM service. Serves OpenAI shape at `/chat/completions`
/// and Anthropic shape at `/messages`, echoing the user content back.
const MOCK_BASE_URL: &str = "https://mock-ai.fly.dev";

// MockAI routes by SDK User-Agent: an agent containing `OpenAI` maps to the
// OpenAI API, one containing `Anthropic` (and not `OpenAI`) maps to the
// Anthropic API, and unknown agents are rejected. So each test uses a
// `RealHttp` whose `User-Agent` matches the backend under test.

/// Live transport that MockAI routes to its OpenAI-compatible endpoint.
fn openai_http() -> RealHttp {
    RealHttp::with_user_agent("claw-api-itest OpenAI/1.0")
}

/// Live transport that MockAI routes to its Anthropic endpoint.
fn anthropic_http() -> RealHttp {
    RealHttp::with_user_agent("claw-api-itest Anthropic/1.0")
}

/// An OpenAI-compatible provider: only `model`/`base_url` differ between them.
struct Provider {
    name: &'static str,
    model: &'static str,
    /// The provider's real base URL (for reference; tests hit the mock instead).
    real_base_url: &'static str,
}

const OPENAI_COMPATIBLE_PROVIDERS: &[Provider] = &[
    Provider {
        name: "OpenAI",
        model: "gpt-4o-mini",
        real_base_url: "https://api.openai.com/v1",
    },
    Provider {
        name: "DeepSeek",
        model: "deepseek-chat",
        real_base_url: "https://api.deepseek.com",
    },
    Provider {
        name: "Qwen",
        model: "qwen-plus",
        real_base_url: "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
    },
    Provider {
        name: "MiniMax",
        model: "MiniMax-Text-01",
        real_base_url: "https://api.minimaxi.com/v1",
    },
    Provider {
        name: "Kimi",
        model: "moonshot-v1-8k",
        real_base_url: "https://api.moonshot.cn/v1",
    },
    Provider {
        name: "GLM",
        model: "glm-4",
        real_base_url: "https://open.bigmodel.cn/api/paas/v4",
    },
];

fn openai_compatible_api(model: &str) -> ClawApi<RealHttp> {
    let mut api = ClawApi::new(openai_http());
    api.set_config(ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "mock-key",
        model,
        MOCK_BASE_URL,
    ))
    .expect("configure openai_compatible");
    api
}

#[test]
#[ignore = "hits hosted mock LLM endpoint over the network; run with --ignored"]
fn openai_compatible_providers_roundtrip() {
    for provider in OPENAI_COMPATIBLE_PROVIDERS {
        // The real base URL is recorded only for documentation.
        assert!(
            provider.real_base_url.starts_with("https://"),
            "{} should document a real https base url",
            provider.name
        );

        let mut api = openai_compatible_api(provider.model);
        // MockAI echoes the last user message content, so a unique marker lets
        // us assert the full request->transport->response->parse round-trip.
        let marker = format!("roundtrip-{}", provider.name);
        let messages = json!([{ "role": "user", "content": marker }]);
        let abort = AtomicBool::new(false);

        let resp = api
            .chat(&ChatRequest::new("be an echo", &messages), &abort)
            .unwrap_or_else(|e| panic!("{} chat failed: {e}", provider.name));

        assert_eq!(
            resp.text.as_deref(),
            Some(marker.as_str()),
            "{} should echo the user content back",
            provider.name
        );
    }
}

#[test]
#[ignore = "hits hosted mock LLM endpoint over the network; run with --ignored"]
fn anthropic_roundtrip() {
    let mut api = ClawApi::new(anthropic_http());
    api.set_config(ClawApiConfig::new(
        BackendKind::AnthropicCompatible,
        "mock-key",
        "claude-3-5-sonnet",
        MOCK_BASE_URL,
    ))
    .expect("configure anthropic_compatible");

    let messages = json!([{ "role": "user", "content": "roundtrip-Anthropic" }]);
    let abort = AtomicBool::new(false);

    let resp = api
        .chat(&ChatRequest::new("be an echo", &messages), &abort)
        .expect("anthropic chat failed");

    assert_eq!(resp.text.as_deref(), Some("roundtrip-Anthropic"));
}

#[test]
#[ignore = "hits hosted mock LLM endpoint over the network; run with --ignored"]
fn chat_json_roundtrip() {
    #[derive(serde::Deserialize)]
    struct Sentiment {
        label: String,
        score: i32,
    }

    let mut api = openai_compatible_api("gpt-4o-mini");

    // MockAI echoes the user content verbatim, so sending a JSON object string
    // lets us exercise the chat_json parse path end-to-end.
    let payload = r#"{"label":"positive","score":1}"#;
    let messages = json!([{ "role": "user", "content": payload }]);
    let schema = r#"{
        "type": "object",
        "properties": { "label": { "type": "string" }, "score": { "type": "integer" } },
        "required": ["label", "score"]
    }"#;
    let abort = AtomicBool::new(false);

    let resp = api
        .chat_json::<Sentiment>(
            &ChatJsonRequest::new("classify", &messages).with_output_schema("sentiment", schema),
            &abort,
        )
        .expect("chat_json failed");

    let output = resp.output.expect("expected parsed JSON output");
    assert_eq!(output.label, "positive");
    assert_eq!(output.score, 1);
}
