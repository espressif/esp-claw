//! Prompt helpers for structured JSON chat (schema-in-prompt fallback).

use serde_json::Value;

/// Append a JSON-schema instruction block to the system prompt (prompt fallback).
pub fn augment_system_with_schema(system_prompt: &str, schema: &Value) -> String {
    let schema_text = serde_json::to_string(schema).unwrap_or_else(|_| "{}".to_string());
    format!(
        "{system_prompt}\n\nRespond with a single JSON object matching this schema (no markdown, no prose):\n{schema_text}"
    )
}
