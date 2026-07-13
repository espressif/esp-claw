//! [`LlmExtractor`] — an [`Extractor`] backed by [`ClawApiAsync`].
//!
//! It asks the model to read a conversation transcript and return a JSON array of
//! durable facts. Like [`LlmCompactor`](crate::memory::LlmCompactor), it lives in
//! `claw_core` (the agent wiring layer) rather than `claw-memory`, because the
//! [`Extractor`] seam stays free of any LLM dependency; the concrete extractor is
//! injected into the long-term memory adapter.

use std::sync::atomic::AtomicBool;
use std::sync::{Arc, RwLock};

use serde_json::{json, Value};

use claw_api::{ChatRequest, ClawApiAsync};
use claw_interface::{Cancel, ClawHttp, ClawTimer};
use claw_memory::MemoryId;
use tracing::Instrument as _;

use crate::config::{ApiUsage, ClawApiManager};
use crate::memory::async_llm::SharedAsyncLlm;

use super::extraction::{
    ExtractError, ExtractFuture, ExtractedItem, ExtractionInput, Extractor, MemoryOp,
    MemorySnapshot,
};

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
pub(crate) struct LlmExtractor<H: ClawHttp, Timer: ClawTimer> {
    api: SharedAsyncLlm<H, Timer>,
    /// Shared per-usage config; the extraction config is applied at the start of
    /// each extraction call.
    api_manager: Arc<RwLock<ClawApiManager>>,
}

impl<H: ClawHttp + Default + 'static, Timer: ClawTimer + Default + 'static> LlmExtractor<H, Timer> {
    /// Build an extractor with its own unconfigured LLM client.
    fn new(api_manager: Arc<RwLock<ClawApiManager>>) -> Self {
        Self {
            api: SharedAsyncLlm::new(ClawApiAsync::new(H::default(), Timer::default())),
            api_manager,
        }
    }

    /// A ready-to-inject [`Extractor`] using `api_manager`.
    pub(crate) fn shared(api_manager: Arc<RwLock<ClawApiManager>>) -> Arc<dyn Extractor> {
        Arc::new(Self::new(api_manager))
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
            let request = ChatRequest::new(EXTRACT_SYSTEM_PROMPT, &messages);
            let max_attempts = u64::from(request.retry.max_retries).saturating_add(1);
            let chat_span =
                tracing::info_span!("api.chat", purpose = "memory_extraction", max_attempts,);
            let response = async {
                let mut lease = self.api.lease().await;
                // Apply this operation's config from the manager (its explicit
                // binding, else the default). None / invalid keeps the current one.
                if let Some(config) = self
                    .api_manager
                    .read()
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .get_api(ApiUsage::Memory)
                {
                    let _ = lease.api_mut().set_config(config);
                }
                lease.api_mut().chat(&request, Cancel::new(&abort)).await
            }
            .instrument(chat_span)
            .await
            .map_err(ExtractError::from)?;

            let Some(text) = response.text else {
                return Err(ExtractError::EmptyOutput);
            };
            if text.trim().is_empty() {
                return Err(ExtractError::EmptyOutput);
            }
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
/// Best-effort: a reply with no parseable array yields no ops, and malformed
/// individual entries are skipped rather than failing the whole batch.
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
        .and_then(Value::as_str)?
        .to_ascii_lowercase();
    match op.as_str() {
        "forget" => Some(MemoryOp::Forget {
            id: parse_id(entry)?,
        }),
        "replace" => Some(MemoryOp::Replace {
            id: parse_id(entry)?,
            item: parse_item(entry)?,
        }),
        "add" => Some(MemoryOp::Add(parse_item(entry)?)),
        _ => None,
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
        tags: string_array(entry.get("tags"))?,
        keywords: string_array(entry.get("keywords"))?,
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

/// Read an optional JSON string array. Missing is empty; malformed is rejected.
fn string_array(value: Option<&Value>) -> Option<Vec<String>> {
    let Some(value) = value else {
        return Some(Vec::new());
    };
    let items = value.as_array()?;
    let mut strings = Vec::with_capacity(items.len());
    for item in items {
        let text = item.as_str()?.trim();
        if !text.is_empty() {
            strings.push(text.to_string());
        }
    }
    Some(strings)
}
