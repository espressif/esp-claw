//! [`LlmCompactor`] — a [`Compactor`] backed by [`ClawApi`].
//!
//! It flattens the aged window into a plain `role: content` transcript and asks
//! the model for a concise recap, returned as a single `system` summary message.
//!
//! This lives in `claw_core` (the agent wiring layer) rather than in
//! `claw-memory`: `claw-memory` owns only the [`Compactor`] *seam* and stays free
//! of any LLM dependency, exactly like it depends on the `ClawThread` /  `ClawFs`
//! traits and never on their implementations. The concrete compactor is injected
//! into `ConversationDeps.compactor` by whoever builds the agent's memory.

use std::sync::atomic::AtomicBool;
use std::sync::Mutex;

use serde_json::{json, Value};

use claw_api::{ChatRequest, ClawApi};
use claw_interface::http::ClawHttp;
use claw_memory::{CompactError, Compactor};

/// System prompt steering the summarization.
const SUMMARY_SYSTEM_PROMPT: &str = "You compress conversation history. Produce a \
concise, faithful summary of the conversation so far, preserving decisions, \
facts, user intent, open questions, and any tool results needed to keep going. \
Do not invent details. Output plain text only.";

/// Instruction prefacing the transcript handed to the model.
const SUMMARY_USER_PREFIX: &str = "Summarize the following conversation transcript:";

/// A [`Compactor`] that summarizes the aged window via the LLM client.
///
/// Owns its own [`ClawApi`] transport (`H`) behind a [`Mutex`]: the compactor is
/// shared across every agent's conversation memory as an `Arc<dyn Compactor>`,
/// while [`ClawApi::chat`] needs `&mut self`, so the mutex serializes the
/// (infrequent, off-tick) compaction calls.
pub struct LlmCompactor<H: ClawHttp> {
    api: Mutex<ClawApi<H>>,
}

impl<H: ClawHttp> LlmCompactor<H> {
    /// Build a compactor that owns the given LLM client.
    ///
    /// The `api` is its own [`ClawApi`] (with its own transport `H`), wired into
    /// `ConversationDeps.compactor` so compaction summarizes through the
    /// configured backend.
    ///
    /// # Examples
    ///
    /// ```
    /// use std::sync::Arc;
    ///
    /// use claw_api::{ClawApi, ClawApiConfig};
    /// use claw_core::LlmCompactor;
    /// use claw_memory::Compactor;
    /// # use std::sync::atomic::AtomicBool;
    /// # use claw_interface::http::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    /// # #[derive(Default)]
    /// # struct StubHttp;
    /// # impl ClawHttp for StubHttp {
    /// #     fn post_json(&mut self, _: &HttpJsonRequest, _: &AtomicBool) -> Result<HttpResponse, HttpError> {
    /// #         Ok(HttpResponse { status_code: 200, body: "{}".into() })
    /// #     }
    /// # }
    /// let api = ClawApi::init(
    ///     ClawApiConfig {
    ///         backend_type: "openai_compatible".into(),
    ///         api_key: Some("sk-test".into()),
    ///         model: Some("gpt-4o-mini".into()),
    ///         base_url: Some("https://api.openai.com/v1".into()),
    ///         ..Default::default()
    ///     },
    ///     StubHttp::default(),
    /// )
    /// .expect("init");
    ///
    /// // A ready-to-inject `Compactor`.
    /// let compactor: Arc<dyn Compactor> = Arc::new(LlmCompactor::new(api));
    /// # let _ = compactor;
    /// ```
    pub fn new(api: ClawApi<H>) -> Self {
        Self {
            api: Mutex::new(api),
        }
    }
}

impl<H: ClawHttp + Send> Compactor for LlmCompactor<H> {
    fn compact(&self, window: &[Value]) -> Result<Vec<Value>, CompactError> {
        let transcript = render_transcript(window);
        let messages = json!([
            { "role": "user", "content": format!("{SUMMARY_USER_PREFIX}\n\n{transcript}") }
        ]);

        // todo: thread a real abort flag once the `Compactor` trait carries one;
        // for now a compaction summarization request is not cancellable.
        let abort = AtomicBool::new(false);
        let response = self
            .api
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .chat(&ChatRequest::new(SUMMARY_SYSTEM_PROMPT, &messages), &abort)
            .map_err(|err| CompactError::Backend(err.to_string()))?;

        let summary = response.text.unwrap_or_default();
        if summary.trim().is_empty() {
            return Err(CompactError::Backend(
                "model returned an empty summary".to_string(),
            ));
        }

        Ok(vec![json!({
            "role": "system",
            "content": format!("Summary of earlier conversation:\n{summary}"),
        })])
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
        let role = message.get("role").and_then(Value::as_str).unwrap_or("?");
        let content = match message.get("content") {
            Some(Value::String(text)) => text.clone(),
            Some(other) => other.to_string(),
            None => String::new(),
        };
        out.push_str(role);
        out.push_str(": ");
        out.push_str(&content);
        out.push('\n');
    }
    out
}
