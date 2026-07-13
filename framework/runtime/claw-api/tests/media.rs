#![allow(clippy::unwrap_used)]

use core::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

use claw_api::{BackendKind, ClawApi, ClawApiConfig, InferMediaError, MediaAsset, MediaRequest};
use claw_interface::http::blocking::ClawHttp as BlockingClawHttp;
use claw_interface::{HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode};
use serde_json::Value;
use tempdir::TempDir;

struct CaptureHttp {
    reply: String,
    last_body: Mutex<Option<String>>,
    last_url: Mutex<Option<String>>,
}

impl CaptureHttp {
    fn new(reply: &str) -> Arc<Self> {
        Arc::new(Self {
            reply: reply.to_string(),
            last_body: Mutex::new(None),
            last_url: Mutex::new(None),
        })
    }
}

struct Owned(Arc<CaptureHttp>);

impl BlockingClawHttp for Owned {
    fn post_json(
        &mut self,
        request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        *self.0.last_body.lock().unwrap() = Some(request.body.to_string());
        *self.0.last_url.lock().unwrap() = Some(request.url.to_string());
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: self.0.reply.clone(),
        })
    }
}

#[test]
fn openai_remote_url_is_sent_as_image_url() {
    let (mut api, http) = openai_api(openai_reply("remote ok"), None);
    let assets = [MediaAsset::remote_url("https://example.com/a.png")];
    let abort = AtomicBool::new(false);

    let text = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap();

    assert_eq!(text, "remote ok");
    assert_eq!(
        http.last_url.lock().unwrap().as_deref(),
        Some("https://api.example.com/v1/chat/completions")
    );
    assert_eq!(
        openai_image_url(&captured_body(&http)),
        "https://example.com/a.png"
    );
}

#[test]
fn openai_local_path_is_sent_as_data_url() {
    let dir = TempDir::new("claw-api-media").unwrap();
    let path = dir.path().join("image.png");
    std::fs::write(&path, b"\x89PNG\r\n\x1a\nABCDE").unwrap();
    let (mut api, http) = openai_api(openai_reply("local ok"), None);
    let assets = [MediaAsset::local_path(path.to_string_lossy().into_owned())];
    let abort = AtomicBool::new(false);

    let text = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap();

    assert_eq!(text, "local ok");
    assert!(openai_image_url(&captured_body(&http)).starts_with("data:image/png;base64,"));
}

#[test]
fn local_path_mime_override_bypasses_extension() {
    let dir = TempDir::new("claw-api-media").unwrap();
    let path = dir.path().join("image.bmp");
    std::fs::write(&path, b"bmpdata").unwrap();
    let (mut api, http) = openai_api(openai_reply("override ok"), None);
    let assets =
        [MediaAsset::local_path(path.to_string_lossy().into_owned()).with_mime_type("image/png")];
    let abort = AtomicBool::new(false);

    api.infer_media(
        &MediaRequest::new(&assets).with_user_prompt("describe"),
        &abort,
    )
    .unwrap();

    assert!(openai_image_url(&captured_body(&http)).starts_with("data:image/png;base64,"));
}

#[test]
fn openai_inline_bytes_are_sent_as_data_url() {
    let (mut api, http) = openai_api(openai_reply("inline ok"), None);
    let assets = [MediaAsset::inline_bytes(
        b"\x89PNG\r\n\x1a\nABCDE".to_vec(),
        "image/png",
    )];
    let abort = AtomicBool::new(false);

    let text = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap();

    assert_eq!(text, "inline ok");
    assert!(openai_image_url(&captured_body(&http)).starts_with("data:image/png;base64,"));
}

#[test]
fn media_rejects_empty_remote_url() {
    let (mut api, _http) = openai_api(openai_reply("unused"), None);
    let assets = [MediaAsset::remote_url("")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::MediaUrlEmpty));
}

#[test]
fn media_rejects_empty_path() {
    let (mut api, _http) = openai_api(openai_reply("unused"), None);
    let assets = [MediaAsset::local_path("")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::MediaPathEmpty));
}

#[test]
fn media_rejects_relative_path() {
    let (mut api, _http) = openai_api(openai_reply("unused"), None);
    let assets = [MediaAsset::local_path("rel/a.png")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::MediaPathNotAbsolute));
}

#[test]
fn media_rejects_unknown_extension() {
    let (mut api, _http) = openai_api(openai_reply("unused"), None);
    let assets = [MediaAsset::local_path("/tmp/a.bmp")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::UnsupportedMediaType));
}

#[test]
fn media_rejects_empty_local_file() {
    let dir = TempDir::new("claw-api-media").unwrap();
    let path = dir.path().join("empty.png");
    std::fs::write(&path, b"").unwrap();
    let (mut api, _http) = openai_api(openai_reply("unused"), None);
    let assets = [MediaAsset::local_path(path.to_string_lossy().into_owned())];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::MediaFileEmpty));
}

#[test]
fn media_rejects_inline_bytes_over_size_limit() {
    let (mut api, _http) = openai_api(openai_reply("unused"), Some(50));
    let assets = [MediaAsset::inline_bytes(vec![0u8; 100], "image/png")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::MediaTooLarge));
}

#[test]
fn media_rejects_empty_inline_bytes() {
    let (mut api, _http) = openai_api(openai_reply("unused"), None);
    let assets = [MediaAsset::inline_bytes(Vec::new(), "image/png")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::MediaFileEmpty));
}

#[test]
fn anthropic_requires_local_image_for_remote_url() {
    let http = CaptureHttp::new(r#"{"content":[{"type":"text","text":"unused"}]}"#);
    let mut api = ClawApi::new(Owned(http));
    api.set_config(ClawApiConfig::new(
        BackendKind::AnthropicCompatible,
        "key",
        "claude-x",
        "https://api.anthropic.com/v1",
    ))
    .unwrap();
    let assets = [MediaAsset::remote_url("https://example.com/a.png")];
    let abort = AtomicBool::new(false);

    let error = api
        .infer_media(
            &MediaRequest::new(&assets).with_user_prompt("describe"),
            &abort,
        )
        .unwrap_err();

    assert!(matches!(error, InferMediaError::RequiresLocalImage));
}

fn openai_api(reply: String, image_max_bytes: Option<usize>) -> (ClawApi<Owned>, Arc<CaptureHttp>) {
    let http = CaptureHttp::new(&reply);
    let mut config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "key",
        "model-x",
        "https://api.example.com/v1",
    );
    if let Some(image_max_bytes) = image_max_bytes {
        config.image_max_bytes = image_max_bytes;
    }
    let mut api = ClawApi::new(Owned(Arc::clone(&http)));
    api.set_config(config).unwrap();
    (api, http)
}

fn openai_reply(text: &str) -> String {
    format!(r#"{{"choices":[{{"message":{{"role":"assistant","content":"{text}"}}}}]}}"#)
}

fn captured_body(http: &CaptureHttp) -> Value {
    let body = http.last_body.lock().unwrap();
    serde_json::from_str(body.as_deref().expect("captured body")).unwrap()
}

fn openai_image_url(body: &Value) -> &str {
    body["messages"]
        .as_array()
        .and_then(|messages| messages.last())
        .and_then(|message| message["content"].as_array())
        .and_then(|content| content.get(1))
        .and_then(|image| image["image_url"]["url"].as_str())
        .expect("openai image url")
}
