//! Async [`ClawApiAsync`] surface: `new`, `set_config`, `chat`, `chat_json`, and
//! `infer_media` driven over the injected async [`ClawHttp`] transport and
//! [`ClawTimer`] backoff seam.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-api --example async_client --target x86_64-unknown-linux-gnu
//! ```
//!
//! To stay dependency-free the example ships its own pieces: a stub async
//! transport that resolves in one poll, an immediate [`ClawTimer`], and a
//! minimal spinning `block_on`. On device these are `esp_http_client` (async)
//! and a real runtime timer.

use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::AtomicBool;
use std::task::{Context, Poll, Waker};
use std::time::Duration;

use claw_api::{
    BackendKind, ChatJsonRequest, ChatRequest, ClawApiAsync, ClawApiConfig, MediaAsset,
    MediaRequest,
};
use claw_interface::http::{
    ClawHttp, HttpError, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
};
use claw_interface::{Cancel, ClawTimer, SleepOutcome, TimerFuture};
use serde::Deserialize;
use serde_json::json;

/// Async transport: same body-sniffing canned replies as the blocking example,
/// resolved immediately in a single poll.
#[derive(Default)]
struct StubHttp;

impl ClawHttp for StubHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            let body = if request.body.contains("response_format") {
                r#"{"choices":[{"message":{"role":"assistant",
                    "content":"{\"label\":\"positive\",\"score\":1}"}}]}"#
            } else if request.body.contains("image_url") {
                r#"{"choices":[{"message":{"role":"assistant","content":"A dog."}}]}"#
            } else {
                r#"{"choices":[{"message":{"role":"assistant","content":"Hi from async!"}}]}"#
            };
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body: body.to_string(),
            })
        })
    }
}

/// A [`ClawTimer`] that never actually waits (retry backoff completes at once).
#[derive(Default)]
struct ImmediateTimer;

impl ClawTimer for ImmediateTimer {
    fn sleep<'a>(&'a mut self, _duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
        Box::pin(async move {
            if cancel.is_cancelled() {
                SleepOutcome::Cancelled
            } else {
                SleepOutcome::Completed
            }
        })
    }
}

fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = Pin::from(Box::new(future));
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
            return output;
        }
    }
}

#[derive(Debug, Deserialize)]
struct Sentiment {
    label: String,
    score: i32,
}

const SENTIMENT_SCHEMA: &str = r#"{"type":"object","properties":{"label":{"type":"string"},"score":{"type":"integer"}},
       "required":["label","score"]}"#;

fn main() -> anyhow::Result<()> {
    let config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-demo",
        "gpt-4o-mini",
        "https://api.example.com/v1",
    );
    let mut api = ClawApiAsync::new(StubHttp, ImmediateTimer);
    api.set_config(config)?;

    let abort = AtomicBool::new(false);
    let cancel = Cancel::new(&abort);

    block_on(async {
        // 1. Async plain chat.
        let messages = json!([{ "role": "user", "content": "Hello?" }]);
        let reply = api
            .chat(&ChatRequest::new("be friendly", &messages), cancel)
            .await?;
        println!("chat       -> {:?}", reply.text);

        // 2. Async structured JSON.
        let messages = json!([{ "role": "user", "content": "classify: great!" }]);
        let out = api
            .chat_json::<Sentiment>(
                &ChatJsonRequest::new("classify sentiment", &messages)
                    .with_output_schema("sentiment", SENTIMENT_SCHEMA),
                cancel,
            )
            .await?;
        if let Some(Sentiment { label, score }) = out.output {
            println!("chat_json  -> {label} ({score})");
        }

        // 3. Async media inference over a remote image URL.
        let assets = [MediaAsset::remote_url("https://example.com/dog.png")];
        let media = MediaRequest::new(&assets).with_user_prompt("Describe this.");
        println!("infer      -> {}", api.infer_media(&media, cancel).await?);

        Ok::<(), anyhow::Error>(())
    })?;

    Ok(())
}
