//! [`LlmCompactor`] — a [`Compactor`] backed by [`ClawApiAsync`].
//!
//! It flattens the aged window into a plain `role: content` transcript and asks
//! the model for a concise recap, returned as a single `system` summary message.
//!
//! This lives in `claw_core` (the agent wiring layer) rather than in
//! `claw-memory`: `claw-memory` owns only the [`Compactor`] *seam* and stays free
//! of any LLM dependency, exactly like it depends on the `ClawThread` /  `ClawFs`
//! traits and never on their implementations. The concrete compactor is wired by
//! the agent layer's rolling-summary adapter.

use std::sync::atomic::AtomicBool;

use serde_json::{json, Value};

use claw_api::{ChatRequest, ClawApiAsync};
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_memory::{CompactBackendError, CompactError, CompactFuture, Compactor};

use super::async_llm::SharedAsyncLlm;

/// System prompt steering the summarization.
const SUMMARY_SYSTEM_PROMPT: &str = "You compress conversation history. Produce a \
concise, faithful summary of the conversation so far, preserving decisions, \
facts, user intent, open questions, and any tool results needed to keep going. \
Do not invent details. Output plain text only.";

/// Instruction prefacing the transcript handed to the model.
const SUMMARY_USER_PREFIX: &str = "Summarize the following conversation transcript:";

/// A [`Compactor`] that summarizes the aged window via the LLM client.
///
/// Owns its own async LLM client. The compactor is shared across every agent's
/// rolling-summary adapter as an `Arc<dyn Compactor>`, while
/// [`ClawApiAsync::chat`] needs `&mut self`, so calls borrow the client
/// exclusively without holding a mutex while the future is running.
pub struct LlmCompactor<H: ClawHttp, Timer: ClawTimer> {
    api: SharedAsyncLlm<H, Timer>,
}

impl<H: ClawHttp, Timer: ClawTimer> LlmCompactor<H, Timer> {
    /// Build a compactor that owns the given LLM client.
    ///
    /// The `api` is its own [`ClawApiAsync`] (with its own transport `H`), so
    /// compaction summarizes through the configured backend without sharing the
    /// main conversation client.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// use std::sync::Arc;
    ///
    /// use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig};
    /// # use super::LlmCompactor;
    /// # use claw_interface::http::{BlockingHttpAdapter, HttpError, HttpJsonRequest, HttpResponse, HttpStatusCode};
    /// # use claw_interface::{Cancel, ClawHttp, ImmediateTimer};
    /// # #[derive(Default)]
    /// # struct StubHttp;
    /// # impl ClawHttp for StubHttp {
    /// #     fn post_json<'a>(&'a mut self, _: &'a HttpJsonRequest<'a>, _: Cancel<'a>) -> claw_interface::HttpResponseFuture<'a> {
    /// #         Box::pin(async {
    /// #         Ok(HttpResponse { status_code: HttpStatusCode::OK, body: "{}".into() })
    /// #         })
    /// #     }
    /// # }
    /// let api = ClawApiAsync::<StubHttp, ImmediateTimer>::init_default(
    ///     ClawApiConfig::new(
    ///         BackendKind::OpenAiCompatible,
    ///         "sk-test",
    ///         "gpt-4o-mini",
    ///         "https://api.openai.com/v1",
    ///     ),
    /// )
    /// .expect("init");
    ///
    /// // A ready-to-inject `Compactor`.
    /// let compactor = Arc::new(LlmCompactor::new(api));
    /// # let _ = compactor;
    /// ```
    pub fn new(api: ClawApiAsync<H, Timer>) -> Self {
        Self {
            api: SharedAsyncLlm::new(api),
        }
    }
}

impl<H: ClawHttp, Timer: ClawTimer> Compactor for LlmCompactor<H, Timer> {
    fn compact<'a>(&'a self, window: &'a [Value]) -> CompactFuture<'a> {
        Box::pin(async move {
            let transcript = render_transcript(window);
            let messages = json!([
                { "role": "user", "content": format!("{SUMMARY_USER_PREFIX}\n\n{transcript}") }
            ]);

            // todo: thread a real abort flag once the `Compactor` trait carries one;
            // for now a compaction summarization request is not cancellable.
            let abort = AtomicBool::new(false);
            let mut lease = self.api.lease().await;
            let response = lease
                .api_mut()
                .chat(
                    &ChatRequest::new(SUMMARY_SYSTEM_PROMPT, &messages),
                    Cancel::new(&abort),
                )
                .await
                .map_err(|error| CompactError::Backend(CompactBackendError::new(error)))?;

            let Some(summary) = response.text else {
                return Err(CompactError::EmptySummary);
            };
            if summary.trim().is_empty() {
                return Err(CompactError::EmptySummary);
            }

            Ok(vec![json!({
                "role": "system",
                "content": format!("Summary of earlier conversation:\n{summary}"),
            })])
        })
    }
}

/// Flatten chat messages into a readable `role: content` transcript.
///
/// Summarizing the *text* (rather than replaying the raw messages as a chat)
/// keeps the summarization request itself immune to tool-call pairing rules —
/// the backend never sees an orphaned `tool` message it would reject.
fn render_transcript(window: &[Value]) -> String {
    let mut out = String::new();
    for message in window {
        let Some(role) = message.get("role").and_then(Value::as_str) else {
            continue;
        };
        let content = match message.get("content") {
            Some(Value::String(text)) => text.clone(),
            Some(other) => other.to_string(),
            None => continue,
        };
        if content.is_empty() {
            continue;
        }
        out.push_str(role);
        out.push_str(": ");
        out.push_str(&content);
        out.push('\n');
    }
    out
}
