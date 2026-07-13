//! Streaming SSE parsing: turn a provider's `text/event-stream` body into
//! ordered [`LlmDelta`]s and, at end-of-stream, a reconstructed [`LlmResponse`].
//!
//! Each parser is a **sync** state machine driven by raw byte chunks, kept free
//! of any async/transport concern so it can be unit-tested with byte slices
//! (including frames split mid-chunk and multibyte UTF-8 split across chunk
//! boundaries). The async [`crate::ChatStream`] wrapper only pumps bytes into
//! [`SseParse::push`] and reads deltas back out.
//!
//! Unlike the non-streaming path there is no single assistant-message JSON on
//! the wire, so [`SseParse::finish`] **reconstructs** one in the provider's
//! shape — it is replayed verbatim into the transcript by the agent loop, so it
//! must match what the non-streaming parser produces.
//!
//! Ordering contract (both providers): within one response deltas are emitted
//! `Reasoning* -> Output* -> ToolCall*`, never interleaved. A `ToolCall` is
//! emitted only once its arguments are complete, so it always carries the whole
//! call.

use serde_json::{json, Map, Value};

use super::super::errors::{ChatError, ClawApiError};
#[cfg(feature = "cache_profile")]
use super::super::types::ApiUsage;
use super::super::types::{LlmDelta, LlmResponse, ToolCall};
#[cfg(feature = "cache_profile")]
use super::shared::{parse_anthropic_usage, parse_openai_usage};

#[cfg(feature = "cache_profile")]
fn merge_usage(current: &mut Option<ApiUsage>, incoming: ApiUsage) {
    let aggregate = current.get_or_insert_with(ApiUsage::default);
    if incoming.input_tokens.is_some() {
        aggregate.input_tokens = incoming.input_tokens;
    }
    if incoming.output_tokens.is_some() {
        aggregate.output_tokens = incoming.output_tokens;
    }
    if incoming.cache_read_tokens.is_some() {
        aggregate.cache_read_tokens = incoming.cache_read_tokens;
    }
    if incoming.cache_write_tokens.is_some() {
        aggregate.cache_write_tokens = incoming.cache_write_tokens;
    }
}

/// SSE event separator. Providers frame events with a blank line.
const FRAME_BOUNDARY: &[u8] = b"\n\n";

/// The concrete SSE parser for the selected backend. Lets [`crate::ChatStream`]
/// stay one non-generic type while dispatching to the right provider parser.
pub(crate) enum ProviderSse {
    OpenAi(OpenAiSse),
    Anthropic(AnthropicSse),
}

impl ProviderSse {
    pub(crate) fn push(&mut self, bytes: &[u8], out: &mut Vec<LlmDelta>) -> Result<(), ChatError> {
        match self {
            Self::OpenAi(parser) => parser.push(bytes, out),
            Self::Anthropic(parser) => parser.push(bytes, out),
        }
    }

    pub(crate) fn finish(self) -> Result<LlmResponse, ChatError> {
        match self {
            Self::OpenAi(parser) => parser.finish(),
            Self::Anthropic(parser) => parser.finish(),
        }
    }
}

/// A provider-specific streaming parser.
pub(crate) trait SseParse {
    /// Feed the next raw body chunk, appending newly-produced deltas to `out`.
    /// Partial frames are buffered until a later call completes them.
    fn push(&mut self, bytes: &[u8], out: &mut Vec<LlmDelta>) -> Result<(), ChatError>;

    /// Assemble the final response, reconstructing the replayable assistant
    /// message JSON in the provider's shape.
    fn finish(self) -> Result<LlmResponse, ChatError>
    where
        Self: Sized;
}

/// Buffers raw bytes and yields complete SSE frames (the text between blank
/// lines). Cutting only on the ASCII `"\n\n"` never splits a multibyte code
/// point, so a returned frame is always valid UTF-8.
#[derive(Default)]
struct FrameBuffer {
    buf: Vec<u8>,
}

impl FrameBuffer {
    fn push_bytes(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
    }

    fn next_frame(&mut self) -> Result<Option<String>, ChatError> {
        let Some(idx) = find_subsequence(&self.buf, FRAME_BOUNDARY) else {
            return Ok(None);
        };
        let frame: Vec<u8> = self.buf.drain(..idx + FRAME_BOUNDARY.len()).collect();
        let text = core::str::from_utf8(&frame[..idx]).map_err(|_| ClawApiError::Parse)?;
        Ok(Some(text.to_string()))
    }
}

/// Yield each `data:` payload in an SSE frame (skipping `event:`, comments, and
/// blank continuation lines).
fn data_payloads(frame: &str) -> impl Iterator<Item = &str> {
    frame.split('\n').filter_map(|line| {
        let payload = line.trim_end_matches('\r').strip_prefix("data:")?;
        let payload = payload.trim_start();
        (!payload.is_empty()).then_some(payload)
    })
}

// ---------------------------------------------------------------------------
// OpenAI-compatible
// ---------------------------------------------------------------------------

/// The OpenAI stream terminator payload.
const OPENAI_DONE: &str = "[DONE]";

/// Accumulated state for one streamed OpenAI tool call, keyed by its `index`.
#[derive(Default)]
struct OpenAiToolCall {
    id: String,
    name: String,
    args: String,
    emitted: bool,
}

/// Incremental parser for an OpenAI-compatible `chat/completions` SSE stream.
#[derive(Default)]
pub(crate) struct OpenAiSse {
    frames: FrameBuffer,
    done: bool,
    text: String,
    reasoning: String,
    tool_calls: Vec<OpenAiToolCall>,
    #[cfg(feature = "cache_profile")]
    usage: Option<ApiUsage>,
}

impl OpenAiSse {
    pub(crate) fn new() -> Self {
        Self::default()
    }

    fn process_data(&mut self, payload: &str, out: &mut Vec<LlmDelta>) -> Result<(), ChatError> {
        let value: Value = serde_json::from_str(payload).map_err(|_| ClawApiError::Parse)?;
        #[cfg(feature = "cache_profile")]
        if let Some(usage) = parse_openai_usage(&value) {
            merge_usage(&mut self.usage, usage);
        }
        let Some(delta) = value
            .get("choices")
            .and_then(|choices| choices.get(0))
            .and_then(|choice| choice.get("delta"))
        else {
            return Ok(()); // e.g. a usage-only final chunk carries no delta
        };

        if let Some(reasoning) = delta.get("reasoning_content").and_then(Value::as_str) {
            if !reasoning.is_empty() {
                self.reasoning.push_str(reasoning);
                out.push(LlmDelta::Reasoning(reasoning.to_string()));
            }
        }
        if let Some(content) = delta.get("content").and_then(Value::as_str) {
            if !content.is_empty() {
                self.text.push_str(content);
                out.push(LlmDelta::Output(content.to_string()));
            }
        }
        if let Some(calls) = delta.get("tool_calls").and_then(Value::as_array) {
            for call in calls {
                self.merge_tool_call(call);
            }
        }
        Ok(())
    }

    fn merge_tool_call(&mut self, call: &Value) {
        let index = call.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
        while self.tool_calls.len() <= index {
            self.tool_calls.push(OpenAiToolCall::default());
        }
        let slot = &mut self.tool_calls[index];
        if let Some(id) = call
            .get("id")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
        {
            slot.id = id.to_string();
        }
        if let Some(function) = call.get("function") {
            if let Some(name) = function
                .get("name")
                .and_then(Value::as_str)
                .filter(|s| !s.is_empty())
            {
                slot.name = name.to_string();
            }
            if let Some(args) = function.get("arguments").and_then(Value::as_str) {
                slot.args.push_str(args);
            }
        }
    }

    /// Emit an [`LlmDelta::ToolCall`] for every not-yet-emitted, named call in
    /// index order. OpenAI has no per-call stop, so this runs at `[DONE]` when
    /// all arguments are known complete.
    ///
    /// Per-call emission at each call's own completion is a later refinement for
    /// eager dispatch; batched dispatch only needs the full set here.
    fn flush_tool_calls(&mut self, out: &mut Vec<LlmDelta>) {
        for (index, slot) in self.tool_calls.iter_mut().enumerate() {
            if slot.emitted || slot.name.is_empty() {
                continue;
            }
            slot.emitted = true;
            out.push(LlmDelta::ToolCall {
                index: index as u32,
                id: slot.id.clone(),
                name: slot.name.clone(),
                arguments: slot.args.clone(),
            });
        }
    }
}

impl SseParse for OpenAiSse {
    fn push(&mut self, bytes: &[u8], out: &mut Vec<LlmDelta>) -> Result<(), ChatError> {
        self.frames.push_bytes(bytes);
        while let Some(frame) = self.frames.next_frame()? {
            for payload in data_payloads(&frame) {
                if payload == OPENAI_DONE {
                    self.done = true;
                    self.flush_tool_calls(out);
                } else {
                    self.process_data(payload, out)?;
                }
            }
        }
        Ok(())
    }

    fn finish(self) -> Result<LlmResponse, ChatError> {
        let text = (!self.text.is_empty()).then(|| self.text.clone());
        let reasoning_content = (!self.reasoning.is_empty()).then(|| self.reasoning.clone());
        let tool_calls: Vec<ToolCall> = self
            .tool_calls
            .iter()
            .filter(|slot| !slot.name.is_empty())
            .map(|slot| ToolCall {
                id: slot.id.clone(),
                name: slot.name.clone(),
                arguments_json: slot.args.clone(),
            })
            .collect();

        if text.is_none() && tool_calls.is_empty() {
            return Err(ClawApiError::EmptyResponse.into());
        }

        let mut message = Map::new();
        message.insert("role".to_string(), json!("assistant"));
        message.insert(
            "content".to_string(),
            text.as_ref().map_or(Value::Null, |t| json!(t)),
        );
        if let Some(reasoning) = &reasoning_content {
            message.insert("reasoning_content".to_string(), json!(reasoning));
        }
        if !tool_calls.is_empty() {
            let calls: Vec<Value> = tool_calls
                .iter()
                .map(|call| {
                    json!({
                        "id": call.id,
                        "type": "function",
                        "function": { "name": call.name, "arguments": call.arguments_json },
                    })
                })
                .collect();
            message.insert("tool_calls".to_string(), Value::Array(calls));
        }
        let raw_message_json = serde_json::to_string(&Value::Object(message))
            .map_err(|_| ClawApiError::ApiError("out of memory serializing streamed message"))?;

        Ok(LlmResponse {
            text,
            reasoning_content,
            raw_message_json: Some(raw_message_json),
            tool_calls,
            #[cfg(feature = "cache_profile")]
            usage: self.usage,
        })
    }
}

// ---------------------------------------------------------------------------
// Anthropic
// ---------------------------------------------------------------------------

/// One content block in an Anthropic assistant message, by block index.
enum AnthBlock {
    Thinking {
        text: String,
        signature: String,
    },
    Text {
        text: String,
    },
    ToolUse {
        ordinal: u32,
        id: String,
        name: String,
        args: String,
    },
    Other,
}

/// Incremental parser for an Anthropic Messages API SSE stream.
#[derive(Default)]
pub(crate) struct AnthropicSse {
    frames: FrameBuffer,
    done: bool,
    /// Content blocks by their Anthropic content-block index (contiguous).
    blocks: Vec<AnthBlock>,
    /// Number of `tool_use` blocks started so far (their emitted ordinal).
    tool_count: u32,
    #[cfg(feature = "cache_profile")]
    usage: Option<ApiUsage>,
}

impl AnthropicSse {
    pub(crate) fn new() -> Self {
        Self::default()
    }

    fn process_data(&mut self, payload: &str, out: &mut Vec<LlmDelta>) -> Result<(), ChatError> {
        let value: Value = serde_json::from_str(payload).map_err(|_| ClawApiError::Parse)?;
        #[cfg(feature = "cache_profile")]
        if let Some(usage) = parse_anthropic_usage(&value) {
            merge_usage(&mut self.usage, usage);
        }
        match value.get("type").and_then(Value::as_str) {
            Some("content_block_start") => self.on_block_start(&value),
            Some("content_block_delta") => self.on_block_delta(&value, out),
            Some("content_block_stop") => self.on_block_stop(&value, out),
            Some("message_stop") => self.done = true,
            _ => {} // message_start / message_delta / ping: ignored
        }
        Ok(())
    }

    fn slot(&mut self, index: usize) -> &mut AnthBlock {
        while self.blocks.len() <= index {
            self.blocks.push(AnthBlock::Other);
        }
        &mut self.blocks[index]
    }

    fn on_block_start(&mut self, value: &Value) {
        let index = block_index(value);
        let kind = value
            .get("content_block")
            .and_then(|b| b.get("type"))
            .and_then(Value::as_str);
        let block = match kind {
            Some("thinking") => AnthBlock::Thinking {
                text: String::new(),
                signature: String::new(),
            },
            Some("text") => AnthBlock::Text {
                text: String::new(),
            },
            Some("tool_use") => {
                let ordinal = self.tool_count;
                self.tool_count += 1;
                let content_block = value.get("content_block");
                AnthBlock::ToolUse {
                    ordinal,
                    id: content_block
                        .and_then(|b| b.get("id"))
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                    name: content_block
                        .and_then(|b| b.get("name"))
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                    args: String::new(),
                }
            }
            _ => AnthBlock::Other,
        };
        *self.slot(index) = block;
    }

    fn on_block_delta(&mut self, value: &Value, out: &mut Vec<LlmDelta>) {
        let index = block_index(value);
        let Some(delta) = value.get("delta") else {
            return;
        };
        match delta.get("type").and_then(Value::as_str) {
            Some("thinking_delta") => {
                if let Some(fragment) = delta.get("thinking").and_then(Value::as_str) {
                    if !fragment.is_empty() {
                        if let AnthBlock::Thinking { text, .. } = self.slot(index) {
                            text.push_str(fragment);
                        }
                        out.push(LlmDelta::Reasoning(fragment.to_string()));
                    }
                }
            }
            Some("signature_delta") => {
                if let Some(sig) = delta.get("signature").and_then(Value::as_str) {
                    if let AnthBlock::Thinking { signature, .. } = self.slot(index) {
                        signature.push_str(sig);
                    }
                }
            }
            Some("text_delta") => {
                if let Some(fragment) = delta.get("text").and_then(Value::as_str) {
                    if !fragment.is_empty() {
                        if let AnthBlock::Text { text } = self.slot(index) {
                            text.push_str(fragment);
                        }
                        out.push(LlmDelta::Output(fragment.to_string()));
                    }
                }
            }
            Some("input_json_delta") => {
                if let Some(fragment) = delta.get("partial_json").and_then(Value::as_str) {
                    if let AnthBlock::ToolUse { args, .. } = self.slot(index) {
                        args.push_str(fragment);
                    }
                }
            }
            _ => {}
        }
    }

    fn on_block_stop(&mut self, value: &Value, out: &mut Vec<LlmDelta>) {
        let index = block_index(value);
        // A tool call's arguments are complete at its block stop — emit it now.
        if let Some(AnthBlock::ToolUse {
            ordinal,
            id,
            name,
            args,
        }) = self.blocks.get(index)
        {
            if !name.is_empty() {
                out.push(LlmDelta::ToolCall {
                    index: *ordinal,
                    id: id.clone(),
                    name: name.clone(),
                    arguments: args.clone(),
                });
            }
        }
    }
}

impl SseParse for AnthropicSse {
    fn push(&mut self, bytes: &[u8], out: &mut Vec<LlmDelta>) -> Result<(), ChatError> {
        self.frames.push_bytes(bytes);
        while let Some(frame) = self.frames.next_frame()? {
            for payload in data_payloads(&frame) {
                self.process_data(payload, out)?;
            }
        }
        Ok(())
    }

    fn finish(self) -> Result<LlmResponse, ChatError> {
        let mut text = String::new();
        let mut reasoning = String::new();
        let mut tool_calls: Vec<ToolCall> = Vec::new();
        let mut content: Vec<Value> = Vec::new();

        for block in &self.blocks {
            match block {
                AnthBlock::Thinking { text: t, signature } if !t.is_empty() => {
                    reasoning.push_str(t);
                    let mut b = Map::new();
                    b.insert("type".to_string(), json!("thinking"));
                    b.insert("thinking".to_string(), json!(t));
                    if !signature.is_empty() {
                        b.insert("signature".to_string(), json!(signature));
                    }
                    content.push(Value::Object(b));
                }
                AnthBlock::Text { text: t } if !t.is_empty() => {
                    text.push_str(t);
                    content.push(json!({ "type": "text", "text": t }));
                }
                AnthBlock::ToolUse { id, name, args, .. } if !name.is_empty() => {
                    let input: Value = serde_json::from_str(args).unwrap_or_else(|_| json!({}));
                    let arguments_json = serde_json::to_string(&input)
                        .map_err(|_| ClawApiError::ApiError("out of memory copying tool call"))?;
                    tool_calls.push(ToolCall {
                        id: id.clone(),
                        name: name.clone(),
                        arguments_json,
                    });
                    content.push(json!({
                        "type": "tool_use",
                        "id": id,
                        "name": name,
                        "input": input,
                    }));
                }
                _ => {}
            }
        }

        let text_opt = (!text.is_empty()).then_some(text);
        let reasoning_opt = (!reasoning.is_empty()).then_some(reasoning);
        if text_opt.is_none() && tool_calls.is_empty() && reasoning_opt.is_none() {
            return Err(ClawApiError::EmptyResponse.into());
        }

        let raw_message_json = serde_json::to_string(&json!({
            "role": "assistant",
            "content": Value::Array(content),
        }))
        .map_err(|_| ClawApiError::ApiError("out of memory copying raw message"))?;

        Ok(LlmResponse {
            text: text_opt,
            reasoning_content: reasoning_opt,
            raw_message_json: Some(raw_message_json),
            tool_calls,
            #[cfg(feature = "cache_profile")]
            usage: self.usage,
        })
    }
}

fn block_index(value: &Value) -> usize {
    value.get("index").and_then(Value::as_u64).unwrap_or(0) as usize
}

/// Index of the first occurrence of `needle` in `haystack`, if any.
fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn drive<P: SseParse>(parser: &mut P, body: &str) -> Vec<LlmDelta> {
        let mut out = Vec::new();
        parser.push(body.as_bytes(), &mut out).unwrap();
        out
    }

    // ----- OpenAI -----

    #[test]
    fn openai_emits_reasoning_then_output_then_tool_in_order() {
        let mut parser = OpenAiSse::new();
        let body = concat!(
            "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}\n\n",
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n",
            "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n",
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"foo\",\"arguments\":\"\"}}]}}]}\n\n",
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"a\\\":1}\"}}]}}]}\n\n",
            "data: [DONE]\n\n",
        );
        let deltas = drive(&mut parser, body);
        assert_eq!(
            deltas,
            vec![
                LlmDelta::Reasoning("think".to_string()),
                LlmDelta::Output("Hel".to_string()),
                LlmDelta::Output("lo".to_string()),
                LlmDelta::ToolCall {
                    index: 0,
                    id: "call_1".to_string(),
                    name: "foo".to_string(),
                    arguments: "{\"a\":1}".to_string(),
                },
            ]
        );
        let response = parser.finish().unwrap();
        assert_eq!(response.text.as_deref(), Some("Hello"));
        assert_eq!(response.reasoning_content.as_deref(), Some("think"));
        assert_eq!(response.tool_calls[0].name, "foo");
        assert_eq!(response.tool_calls[0].arguments_json, "{\"a\":1}");
        let raw: Value =
            serde_json::from_str(response.raw_message_json.as_deref().unwrap()).unwrap();
        assert_eq!(raw["role"], "assistant");
        assert_eq!(raw["content"], "Hello");
        assert_eq!(raw["tool_calls"][0]["function"]["name"], "foo");
        assert_eq!(raw["tool_calls"][0]["function"]["arguments"], "{\"a\":1}");
    }

    #[test]
    fn openai_reassembles_frames_split_across_chunks() {
        let mut parser = OpenAiSse::new();
        let mut out = Vec::new();
        let full = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n";
        let (a, b) = full.split_at(10);
        let (b, c) = b.split_at(15);
        parser.push(a.as_bytes(), &mut out).unwrap();
        assert!(out.is_empty());
        parser.push(b.as_bytes(), &mut out).unwrap();
        assert!(out.is_empty());
        parser.push(c.as_bytes(), &mut out).unwrap();
        assert_eq!(out, vec![LlmDelta::Output("hi".to_string())]);
    }

    #[test]
    fn openai_reassembles_multibyte_utf8_split_across_chunks() {
        let mut parser = OpenAiSse::new();
        let mut out = Vec::new();
        let full = "data: {\"choices\":[{\"delta\":{\"content\":\"上\"}}]}\n\n";
        let bytes = full.as_bytes();
        let cut = full.find('上').unwrap() + 1;
        parser.push(&bytes[..cut], &mut out).unwrap();
        parser.push(&bytes[cut..], &mut out).unwrap();
        assert_eq!(out, vec![LlmDelta::Output("上".to_string())]);
    }

    #[test]
    fn openai_empty_stream_is_an_error() {
        let mut parser = OpenAiSse::new();
        drive(&mut parser, "data: [DONE]\n\n");
        assert!(parser.finish().is_err());
    }

    #[cfg(feature = "cache_profile")]
    #[test]
    fn openai_captures_usage_only_final_chunk() {
        let mut parser = OpenAiSse::new();
        let body = concat!(
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n",
            "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3,\"prompt_tokens_details\":{\"cached_tokens\":8}}}\n\n",
            "data: [DONE]\n\n",
        );
        drive(&mut parser, body);

        let usage = parser.finish().unwrap().usage.unwrap();
        assert_eq!(usage.input_tokens, Some(12));
        assert_eq!(usage.output_tokens, Some(3));
        assert_eq!(usage.cache_read_tokens, Some(8));
        assert_eq!(usage.cache_write_tokens, None);
    }

    // ----- Anthropic -----

    #[test]
    fn anthropic_emits_reasoning_then_output_then_tool_in_order() {
        let mut parser = AnthropicSse::new();
        let body = concat!(
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}\n\n",
            "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n",
            "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n",
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"foo\",\"input\":{}}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"a\\\":\"}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"1}\"}}\n\n",
            "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":2}\n\n",
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
        );
        let deltas = drive(&mut parser, body);
        assert_eq!(
            deltas,
            vec![
                LlmDelta::Reasoning("hmm".to_string()),
                LlmDelta::Output("Hi".to_string()),
                LlmDelta::ToolCall {
                    index: 0,
                    id: "toolu_1".to_string(),
                    name: "foo".to_string(),
                    arguments: "{\"a\":1}".to_string(),
                },
            ]
        );
        let response = parser.finish().unwrap();
        assert_eq!(response.text.as_deref(), Some("Hi"));
        assert_eq!(response.reasoning_content.as_deref(), Some("hmm"));
        assert_eq!(response.tool_calls[0].name, "foo");
        assert_eq!(response.tool_calls[0].arguments_json, "{\"a\":1}");
        let raw: Value =
            serde_json::from_str(response.raw_message_json.as_deref().unwrap()).unwrap();
        assert_eq!(raw["content"][0]["type"], "thinking");
        assert_eq!(raw["content"][1]["type"], "text");
        assert_eq!(raw["content"][2]["type"], "tool_use");
        assert_eq!(raw["content"][2]["name"], "foo");
        assert_eq!(raw["content"][2]["input"]["a"], 1);
    }

    #[test]
    fn anthropic_reassembles_frames_split_across_chunks() {
        let mut parser = AnthropicSse::new();
        let mut out = Vec::new();
        let full = "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n";
        let (a, b) = full.split_at(40);
        parser.push(a.as_bytes(), &mut out).unwrap();
        assert!(out.is_empty());
        parser.push(b.as_bytes(), &mut out).unwrap();
        assert_eq!(out, vec![LlmDelta::Output("hi".to_string())]);
    }

    #[cfg(feature = "cache_profile")]
    #[test]
    fn anthropic_merges_usage_across_stream_events() {
        let mut parser = AnthropicSse::new();
        let body = concat!(
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":20,\"cache_read_input_tokens\":12,\"cache_creation_input_tokens\":8}}}\n\n",
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n",
            "event: message_delta\ndata: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":5}}\n\n",
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
        );
        drive(&mut parser, body);

        let usage = parser.finish().unwrap().usage.unwrap();
        assert_eq!(usage.input_tokens, Some(20));
        assert_eq!(usage.output_tokens, Some(5));
        assert_eq!(usage.cache_read_tokens, Some(12));
        assert_eq!(usage.cache_write_tokens, Some(8));
    }
}
