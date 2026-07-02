//! [`LlmExtractor`] — an [`Extractor`] backed by [`ClawApiAsync`].
//!
//! It asks the model to read a conversation transcript and return a JSON array of
//! durable facts. Like [`LlmCompactor`](crate::memory::LlmCompactor), it lives in
//! `claw_core` (the agent wiring layer) rather than `claw-memory`, because the
//! [`Extractor`] seam stays free of any LLM dependency; the concrete extractor is
//! injected into the long-term memory adapter.

use std::sync::atomic::AtomicBool;
use std::sync::Arc;

use serde_json::{json, Value};

use claw_api::{ChatRequest, ClawApiAsync};
use claw_interface::{Cancel, ClawHttp, ClawTimer};

use super::async_llm::SharedAsyncLlm;
use super::extraction::{ExtractError, ExtractFuture, ExtractedItem, Extractor};

/// System prompt steering the extraction. Asks for stable, third-person facts and
/// nothing else, so the result is parseable JSON.
const EXTRACT_SYSTEM_PROMPT: &str = "You maintain an AI assistant's long-term memory. \
Read the conversation transcript and extract only durable facts worth remembering \
across future conversations: concrete user/device facts, standing factual \
preferences, important commitments, and stable operational context. Do NOT extract \
assistant persona, assistant identity, user profile document edits, tone/style \
instructions, or requests to change long-term behavior; those belong in profile \
documents, not long-term fact memory. Ignore transient chit-chat, one-off requests, \
and anything already obvious. Write each fact concisely in the third person. Respond \
with ONLY a JSON array of objects, each: {\"content\": string, \"tags\": [string], \
\"keywords\": [string]}. Use short topic tags (e.g. \"preference\", \"device\", \
\"fact\"). If nothing is worth remembering, respond with [].";

/// Instruction prefacing the transcript handed to the model.
const EXTRACT_USER_PREFIX: &str = "Extract durable memory from this transcript:";

/// An [`Extractor`] that distills facts via the LLM client.
///
/// Owns its own async LLM client. The extractor is shared across agents as an
/// `Arc<dyn Extractor>`, while [`ClawApiAsync::chat`] needs `&mut self`, so
/// calls borrow the client exclusively without holding a mutex while the future
/// is running.
pub struct LlmExtractor<H: ClawHttp, Timer: ClawTimer> {
    api: SharedAsyncLlm<H, Timer>,
}

impl<H: ClawHttp + 'static, Timer: ClawTimer + 'static> LlmExtractor<H, Timer> {
    /// Build an extractor that owns the given LLM client.
    pub fn new(api: ClawApiAsync<H, Timer>) -> Self {
        Self {
            api: SharedAsyncLlm::new(api),
        }
    }

    /// A ready-to-inject [`Extractor`] over `api`.
    pub fn shared(api: ClawApiAsync<H, Timer>) -> Arc<dyn Extractor> {
        Arc::new(Self::new(api))
    }
}

impl<H: ClawHttp, Timer: ClawTimer> Extractor for LlmExtractor<H, Timer> {
    fn extract<'a>(&'a self, transcript: &'a str) -> ExtractFuture<'a> {
        Box::pin(async move {
            let messages = json!([
                { "role": "user", "content": format!("{EXTRACT_USER_PREFIX}\n\n{transcript}") }
            ]);

            // Extraction is not tied to the active iteration's interrupt flag,
            // so it uses its own (never-set) abort flag.
            let abort = AtomicBool::new(false);
            let mut lease = self.api.lease().await;
            let response = lease
                .api_mut()
                .chat(
                    &ChatRequest::new(EXTRACT_SYSTEM_PROMPT, &messages),
                    Cancel::new(&abort),
                )
                .await
                .map_err(|error| ExtractError::Backend(error.to_string()))?;

            let text = response.text.unwrap_or_default();
            Ok(parse_items(&text))
        })
    }
}

/// Parse the model's reply into items, tolerating prose around the JSON array.
///
/// Best-effort: a reply with no parseable array yields no items (the model
/// decided nothing was worth remembering, or wandered off format); malformed
/// individual entries are skipped rather than failing the whole batch.
fn parse_items(text: &str) -> Vec<ExtractedItem> {
    let Some(array) = extract_json_array(text) else {
        return Vec::new();
    };
    array
        .iter()
        .filter_map(|entry| {
            let content = entry.get("content").and_then(Value::as_str)?.trim();
            if content.is_empty() {
                return None;
            }
            Some(ExtractedItem {
                content: content.to_string(),
                tags: string_array(entry.get("tags")),
                keywords: string_array(entry.get("keywords")),
                // The LLM is intentionally not asked to choose a tier; the
                // classifier decides from the tags.
                tier: None,
            })
        })
        .collect()
}

/// Pull the first top-level JSON array out of `text` (the model may wrap it in
/// prose or a code fence). Returns its elements, or `None` if none parses.
fn extract_json_array(text: &str) -> Option<Vec<Value>> {
    let start = text.find('[')?;
    let end = text.rfind(']')?;
    let slice = text.get(start..=end)?;
    serde_json::from_str::<Value>(slice)
        .ok()
        .and_then(|value| match value {
            Value::Array(items) => Some(items),
            _ => None,
        })
}

/// Read a JSON string array, dropping non-string and empty entries.
fn string_array(value: Option<&Value>) -> Vec<String> {
    value
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .map(str::trim)
                .filter(|item| !item.is_empty())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_clean_array() {
        let text = r#"[{"content":"Likes tea","tags":["preference"],"keywords":["tea"]}]"#;
        let items = parse_items(text);
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].content, "Likes tea");
        assert_eq!(items[0].tags, vec!["preference".to_string()]);
        assert_eq!(items[0].keywords, vec!["tea".to_string()]);
        assert_eq!(items[0].tier, None);
    }

    #[test]
    fn tolerates_prose_around_the_array() {
        let text = "Sure! Here is what I found:\n[{\"content\":\"Name is Ada\"}]\nHope that helps.";
        let items = parse_items(text);
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].content, "Name is Ada");
        assert!(items[0].tags.is_empty());
    }

    #[test]
    fn empty_array_yields_no_items() {
        assert!(parse_items("[]").is_empty());
        assert!(parse_items("nothing worth remembering").is_empty());
    }

    #[test]
    fn skips_entries_without_content() {
        let text = r#"[{"tags":["x"]},{"content":"  "},{"content":"real fact"}]"#;
        let items = parse_items(text);
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].content, "real fact");
    }
}
