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
use claw_memory::MemoryId;

use super::async_llm::SharedAsyncLlm;
use super::extraction::{
    ExtractError, ExtractFuture, ExtractedItem, ExtractionInput, Extractor, MemoryOp,
    MemorySnapshot,
};
use super::MemoryTierHint;

/// System prompt steering the extraction. Asks the model to reconcile memory
/// against the conversation and reply with ONLY a JSON array of ops.
const EXTRACT_SYSTEM_PROMPT: &str = "You maintain an AI assistant's long-term memory. \
You are given the CURRENT MEMORY (each fact prefixed with its id) and a CONVERSATION \
transcript. Decide what durable memory should change. Durable facts worth keeping are: \
concrete user/device facts, standing factual preferences, important commitments, and \
stable operational context. Do NOT store assistant persona, assistant identity, user \
profile document edits, tone/style instructions, or requests to change long-term \
behavior; those belong in profile documents. Ignore transient chit-chat and one-off \
requests. Write each fact concisely in the third person. \
Respond with ONLY a JSON array of operation objects. Each object has an \"op\" field: \
{\"op\":\"add\", \"content\": string, \"tags\": [string], \"keywords\": [string]} to store a NEW fact; \
{\"op\":\"replace\", \"id\": string, \"content\": string, \"tags\": [string], \"keywords\": [string]} \
to update an existing fact the conversation changed; \
{\"op\":\"forget\", \"id\": string} to remove a fact the user retracted or that is now false. \
For \"replace\" and \"forget\", \"id\" MUST be one of the ids shown in CURRENT MEMORY. \
Use short topic tags (e.g. \"preference\", \"device\", \"fact\"). Do not re-add facts \
already present unchanged. If nothing should change, respond with [].";

/// Header prefacing the current-memory listing handed to the model.
const EXTRACT_MEMORY_HEADER: &str = "CURRENT MEMORY:";

/// Header prefacing the transcript handed to the model.
const EXTRACT_TRANSCRIPT_HEADER: &str = "CONVERSATION:";

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
    fn extract<'a>(&'a self, input: ExtractionInput<'a>) -> ExtractFuture<'a> {
        Box::pin(async move {
            let prompt = format!(
                "{EXTRACT_MEMORY_HEADER}\n{}\n\n{EXTRACT_TRANSCRIPT_HEADER}\n{}",
                render_existing(input.existing),
                input.transcript
            );
            let messages = json!([{ "role": "user", "content": prompt }]);

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
            Ok(parse_ops(&text))
        })
    }
}

/// Render the current memory as an `id: content [tags]` listing for the prompt,
/// or `(none)` when empty.
fn render_existing(existing: &[MemorySnapshot]) -> String {
    if existing.is_empty() {
        return "(none)".to_string();
    }
    existing
        .iter()
        .map(|item| {
            if item.tags.is_empty() {
                format!("{}: {}", item.id, item.content)
            } else {
                format!("{}: {} [{}]", item.id, item.content, item.tags.join(", "))
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// Parse the model's reply into ops, tolerating prose around the JSON array.
///
/// Best-effort: a reply with no parseable array yields no ops; a malformed
/// individual entry is skipped rather than failing the whole batch. An entry
/// with no `op` field defaults to `add`. A `replace`/`forget` missing a usable
/// `id` is dropped (it cannot name a target).
fn parse_ops(text: &str) -> Vec<MemoryOp> {
    let Some(array) = extract_json_array(text) else {
        return Vec::new();
    };
    array.iter().filter_map(parse_op).collect()
}

/// Parse one op object, or `None` if it is malformed for its kind.
fn parse_op(entry: &Value) -> Option<MemoryOp> {
    let op = entry
        .get("op")
        .and_then(Value::as_str)
        .unwrap_or("add")
        .to_ascii_lowercase();
    match op.as_str() {
        "forget" => Some(MemoryOp::Forget {
            id: parse_id(entry)?,
        }),
        "replace" => Some(MemoryOp::Replace {
            id: parse_id(entry)?,
            item: parse_item(entry)?,
        }),
        // Default (and explicit "add").
        _ => Some(MemoryOp::Add(parse_item(entry)?)),
    }
}

/// Read the non-empty `id` field as a [`MemoryId`], or `None`.
fn parse_id(entry: &Value) -> Option<MemoryId> {
    let id = entry.get("id").and_then(Value::as_str)?.trim();
    (!id.is_empty()).then(|| MemoryId::from(id))
}

/// Read the `content`/`tags`/`keywords` fields into an [`ExtractedItem`], or
/// `None` when `content` is missing/blank.
fn parse_item(entry: &Value) -> Option<ExtractedItem> {
    let content = entry.get("content").and_then(Value::as_str)?.trim();
    if content.is_empty() {
        return None;
    }
    Some(ExtractedItem {
        content: content.to_string(),
        tags: string_array(entry.get("tags")),
        keywords: string_array(entry.get("keywords")),
        // The LLM is intentionally not asked to choose a tier; the classifier
        // decides from the tags.
        tier: MemoryTierHint::Auto,
    })
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
#[allow(clippy::indexing_slicing)]
mod tests {
    use super::*;

    #[test]
    fn add_op_defaults_when_op_field_absent() {
        let text = r#"[{"content":"Likes tea","tags":["preference"],"keywords":["tea"]}]"#;
        let ops = parse_ops(text);
        assert_eq!(ops.len(), 1);
        match &ops[0] {
            MemoryOp::Add(item) => {
                assert_eq!(item.content, "Likes tea");
                assert_eq!(item.tags, vec!["preference".to_string()]);
                assert_eq!(item.keywords, vec!["tea".to_string()]);
                assert_eq!(item.tier, MemoryTierHint::Auto);
            }
            other => panic!("expected add, got {other:?}"),
        }
    }

    #[test]
    fn explicit_add_replace_and_forget_parse() {
        let text = r#"[
            {"op":"add","content":"Has a dog"},
            {"op":"replace","id":"g-2","content":"Lives in Berlin","tags":["fact"]},
            {"op":"forget","id":"a-5"}
        ]"#;
        let ops = parse_ops(text);
        assert_eq!(ops.len(), 3);
        assert!(matches!(&ops[0], MemoryOp::Add(item) if item.content == "Has a dog"));
        match &ops[1] {
            MemoryOp::Replace { id, item } => {
                assert_eq!(id.as_str(), "g-2");
                assert_eq!(item.content, "Lives in Berlin");
            }
            other => panic!("expected replace, got {other:?}"),
        }
        assert!(matches!(&ops[2], MemoryOp::Forget { id } if id.as_str() == "a-5"));
    }

    #[test]
    fn tolerates_prose_around_the_array() {
        let text = "Sure! Here is what I found:\n[{\"content\":\"Name is Ada\"}]\nHope that helps.";
        let ops = parse_ops(text);
        assert_eq!(ops.len(), 1);
        assert!(matches!(&ops[0], MemoryOp::Add(item) if item.content == "Name is Ada"));
    }

    #[test]
    fn empty_array_yields_no_ops() {
        assert!(parse_ops("[]").is_empty());
        assert!(parse_ops("nothing worth remembering").is_empty());
    }

    #[test]
    fn drops_malformed_entries() {
        // add without content, replace/forget without an id: each is unusable and
        // dropped, leaving only the one real fact.
        let text = r#"[
            {"tags":["x"]},
            {"content":"  "},
            {"op":"replace","content":"no id here"},
            {"op":"forget"},
            {"op":"forget","id":"  "},
            {"content":"real fact"}
        ]"#;
        let ops = parse_ops(text);
        assert_eq!(ops.len(), 1);
        assert!(matches!(&ops[0], MemoryOp::Add(item) if item.content == "real fact"));
    }
}
