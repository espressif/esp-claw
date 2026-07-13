//! Blocking [`ClawApi::infer_media`] surface: build image inputs three ways
//! ([`MediaAsset::local_path`], [`MediaAsset::remote_url`],
//! [`MediaAsset::inline_bytes`]), inspect each enum variant's payload, and send
//! them with every [`MediaRequest`] builder.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-api --example media --target x86_64-unknown-linux-gnu
//! ```
//!
//! The transport is a stub returning a canned vision reply, so no network or
//! real model is involved. The local-file path is exercised against a tiny
//! temporary `.png` written to the OS temp dir.

use std::sync::atomic::AtomicBool;

use claw_api::{BackendKind, ClawApi, ClawApiConfig, MediaAsset, MediaRequest, RetryPolicy};
use claw_interface::http::{
    blocking::ClawHttp, HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode,
};

/// Canned transport: returns an OpenAI-shaped chat completion describing the
/// image, regardless of the (base64 / URL) payload the pipeline built.
struct StubHttp;

impl ClawHttp for StubHttp {
    fn post_json(
        &mut self,
        _request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body:
                r#"{"choices":[{"message":{"role":"assistant","content":"A small red square."}}]}"#
                    .to_string(),
        })
    }
}

/// Print the variant + payload of a [`MediaAsset`].
fn describe(asset: &MediaAsset) {
    let detail = match asset {
        MediaAsset::LocalPath { path, mime_type } => {
            format!("LocalPath   path={path} mime={mime_type:?}")
        }
        MediaAsset::RemoteUrl { url } => format!("RemoteUrl   url={url}"),
        MediaAsset::InlineBytes { bytes, mime_type } => {
            format!("InlineBytes bytes={}B mime={mime_type}", bytes.len())
        }
    };
    println!("asset      -> {detail}");
}

fn main() -> anyhow::Result<()> {
    // A tiny on-disk PNG so the local-path branch (read + base64) actually runs.
    let mut png_path = std::env::temp_dir();
    png_path.push("claw_api_example.png");
    std::fs::write(&png_path, b"\x89PNG\r\n\x1a\nfake-image-bytes")?;
    let png_path = png_path.to_string_lossy().into_owned();

    // Three ways to supply an image; `with_mime_type` overrides the inferred type.
    let local = MediaAsset::local_path(&png_path);
    let remote = MediaAsset::remote_url("https://example.com/cat.png");
    let inline = MediaAsset::inline_bytes(b"\x89PNG\r\n\x1a\ninline".to_vec(), "image/png")
        .with_mime_type("image/png");
    for asset in [&local, &remote, &inline] {
        describe(asset);
    }

    let config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-demo",
        "gpt-4o-mini",
        "https://api.example.com/v1",
    );
    let mut api = ClawApi::new(StubHttp);
    api.set_config(config)?;
    let abort = AtomicBool::new(false);

    // A media request with both prompts and a custom retry policy; the fields
    // are readable back through the public struct.
    let assets = [local];
    let request = MediaRequest::new(&assets)
        .with_system_prompt("You are a vision assistant.")
        .with_user_prompt("Describe this image.")
        .with_retry(RetryPolicy::new(1));
    println!(
        "request    -> media={} system={:?} user={:?} retries={}",
        request.media.len(),
        request.system_prompt,
        request.user_prompt,
        request.retry.max_retries
    );

    let description = api.infer_media(&request, &abort)?;
    println!("infer      -> {description}");

    // The remote-URL asset takes the pass-through branch (no local file read).
    let remote_assets = [remote];
    let remote_request = MediaRequest::new(&remote_assets).with_user_prompt("What is this?");
    println!(
        "remote     -> {}",
        api.infer_media(&remote_request, &abort)?
    );

    let _ = std::fs::remove_file(&png_path);
    Ok(())
}
