//! LLM-backed transformation for one aged conversation-history window.

use std::sync::atomic::AtomicBool;
use std::sync::{Arc, RwLock};

use claw_api::{ChatRequest, ClawApiAsync};
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_memory::{CompactBackendError, CompactError, CompactFuture, Compactor};
use serde_json::{json, Value};
use tracing::Instrument as _;

use crate::config::{ApiUsage, ClawApiManager};
use crate::memory::async_llm::SharedAsyncLlm;

const SUMMARY_SYSTEM_PROMPT: &str = "You compress conversation history. Produce a \
concise, faithful summary of the conversation so far, preserving decisions, \
facts, user intent, open questions, and any tool results needed to keep going. \
Do not invent details. Output plain text only.";

const SUMMARY_USER_PREFIX: &str = "Summarize the following conversation transcript:";

/// A [`Compactor`] that summarizes an aged history window via the LLM client.
pub(crate) struct LlmCompactor<H: ClawHttp, Timer: ClawTimer> {
    api: SharedAsyncLlm<H, Timer>,
    api_manager: Arc<RwLock<ClawApiManager>>,
}

impl<H: ClawHttp + Default, Timer: ClawTimer + Default> LlmCompactor<H, Timer> {
    pub(crate) fn new(api_manager: Arc<RwLock<ClawApiManager>>) -> Self {
        Self {
            api: SharedAsyncLlm::new(ClawApiAsync::new(H::default(), Timer::default())),
            api_manager,
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

            // todo: thread a real abort flag once `Compactor` carries one.
            let abort = AtomicBool::new(false);
            let request = ChatRequest::new(SUMMARY_SYSTEM_PROMPT, &messages);
            let max_attempts = u64::from(request.retry.max_retries).saturating_add(1);
            let chat_span = tracing::info_span!(
                "api.chat",
                purpose = "conversation_compaction",
                max_attempts,
            );
            let response = async {
                let mut lease = self.api.lease().await;
                if let Some(config) = self
                    .api_manager
                    .read()
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .get_api(ApiUsage::Compaction)
                {
                    let _ = lease.api_mut().set_config(config);
                }
                lease.api_mut().chat(&request, Cancel::new(&abort)).await
            }
            .instrument(chat_span)
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
