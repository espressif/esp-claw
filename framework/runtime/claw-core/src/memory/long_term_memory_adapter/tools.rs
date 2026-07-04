//! The five model-callable long-term memory tools, presented as one unified
//! surface over the dual-tier store.
//!
//! None of these expose a tier/scope parameter: `memory_store` routes through the
//! [`TierClassifier`](crate::memory::TierClassifier), and `memory_recall` /
//! `memory_list` merge both tiers. The model sees a single memory it can write,
//! read, edit, and forget. `memory_update` / `memory_forget` take the opaque id
//! returned by recall/list, which the adapter routes back to the owning store by
//! its prefix.

use claw_capability::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};
use claw_interface::ClawFs;
use claw_memory::{MemoryDraft, MemoryId, MemoryItem, MemoryPatch, StoreOutcome};
use serde_json::Value;

use super::{MemoryStores, MemoryTierHint};

/// Default `memory_recall` / `memory_list` result cap when the model omits one.
const DEFAULT_RECALL_LIMIT: usize = 20;

/// Build the long-term memory tools over the shared stores.
pub(crate) fn memory_tools<F: ClawFs + 'static>(stores: MemoryStores<F>) -> Vec<Tool> {
    vec![
        Tool::from_sync(MemoryStoreTool {
            stores: stores.clone(),
        }),
        Tool::from_sync(MemoryRecallTool {
            stores: stores.clone(),
        }),
        Tool::from_sync(MemoryListTool {
            stores: stores.clone(),
        }),
        Tool::from_sync(MemoryUpdateTool {
            stores: stores.clone(),
        }),
        Tool::from_sync(MemoryForgetTool { stores }),
    ]
}

// -- memory_store -----------------------------------------------------------

struct MemoryStoreTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryStoreTool<F> {
    tool_metadata!("memory_store");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryStoreTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let content = required_string(&args, "content")?;
        let draft = MemoryDraft::new(content)
            .with_tags(string_array(&args, "tags"))
            .with_keywords(string_array(&args, "keywords"))
            .with_source("manual");

        let output = match self.stores.store(draft, MemoryTierHint::Auto) {
            StoreOutcome::Created(item) => format!("Stored memory {}.", item.id),
            StoreOutcome::Duplicate(item) => {
                format!("Already remembered (as {}); nothing changed.", item.id)
            }
        };
        Ok(ToolOutput { output, ok: true })
    }
}

// -- memory_recall ----------------------------------------------------------

struct MemoryRecallTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryRecallTool<F> {
    tool_metadata!("memory_recall");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryRecallTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let labels = string_array(&args, "labels");
        let query = optional_string(&args, "query");
        let limit = optional_limit(&args);

        let items = self.stores.recall(&labels, query.as_deref(), limit);
        Ok(ToolOutput {
            output: render_items("Recalled memories", &items),
            ok: true,
        })
    }
}

// -- memory_list ------------------------------------------------------------

struct MemoryListTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryListTool<F> {
    tool_metadata!("memory_list");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryListTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let limit = optional_limit(&args);
        let mut items = self.stores.list();
        items.truncate(limit);
        Ok(ToolOutput {
            output: render_items("All memories", &items),
            ok: true,
        })
    }
}

// -- memory_update ----------------------------------------------------------

struct MemoryUpdateTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryUpdateTool<F> {
    tool_metadata!("memory_update");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryUpdateTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let id = MemoryId::from(required_string(&args, "id")?.as_str());
        let patch = MemoryPatch {
            content: optional_string(&args, "content"),
            tags: optional_string_array(&args, "tags"),
            keywords: optional_string_array(&args, "keywords"),
        };
        match self.stores.update(&id, patch) {
            Ok(item) => Ok(ToolOutput {
                output: format!("Updated memory {}.", item.id),
                ok: true,
            }),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not update {id}: {error}."),
                ok: false,
            }),
        }
    }
}

// -- memory_forget ----------------------------------------------------------

struct MemoryForgetTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryForgetTool<F> {
    tool_metadata!("memory_forget");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryForgetTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let id = MemoryId::from(required_string(&args, "id")?.as_str());
        match self.stores.forget(&id) {
            Ok(()) => Ok(ToolOutput {
                output: format!("Forgot memory {id}."),
                ok: true,
            }),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not forget {id}: {error}."),
                ok: false,
            }),
        }
    }
}

// -- shared argument / rendering helpers ------------------------------------

/// Parse a call's arguments into a JSON object (arguments are already schema-
/// validated by the tool set, but parse defensively).
fn parse_object(call: &ToolInvocation<'_>) -> Result<Value, ToolInvokeError> {
    if call.arguments_json().trim().is_empty() {
        return Ok(Value::Object(serde_json::Map::new()));
    }
    serde_json::from_str(call.arguments_json())
        .map_err(|error| ToolInvokeError::new(ToolError::InvalidArgumentsJson(error.to_string())))
}

/// A required non-empty string field.
fn required_string(args: &Value, key: &str) -> Result<String, ToolInvokeError> {
    let value = args
        .get(key)
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|text| !text.is_empty())
        .ok_or_else(|| {
            ToolInvokeError::new(ToolError::InvokeRejected(format!(
                "missing required string field '{key}'"
            )))
        })?;
    Ok(value.to_string())
}

/// An optional non-empty string field.
fn optional_string(args: &Value, key: &str) -> Option<String> {
    args.get(key)
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|text| !text.is_empty())
        .map(str::to_string)
}

/// A string array field (absent or non-array yields an empty vec).
fn string_array(args: &Value, key: &str) -> Vec<String> {
    optional_string_array(args, key).unwrap_or_default()
}

/// An optional string array field; `None` when the key is absent.
fn optional_string_array(args: &Value, key: &str) -> Option<Vec<String>> {
    args.get(key).and_then(Value::as_array).map(|items| {
        items
            .iter()
            .filter_map(Value::as_str)
            .map(str::trim)
            .filter(|text| !text.is_empty())
            .map(str::to_string)
            .collect()
    })
}

/// The `limit` field, clamped to a sensible default when absent or zero.
fn optional_limit(args: &Value) -> usize {
    args.get("limit")
        .and_then(Value::as_u64)
        .and_then(|limit| usize::try_from(limit).ok())
        .filter(|limit| *limit > 0)
        .unwrap_or(DEFAULT_RECALL_LIMIT)
}

/// Render a list of memory items for the model, one per line with its id and
/// tags so the model can address them in `memory_update` / `memory_forget`.
fn render_items(header: &str, items: &[MemoryItem]) -> String {
    if items.is_empty() {
        return "No matching memories.".to_string();
    }
    let mut out = format!("{header}:\n");
    for item in items {
        out.push_str("- [");
        out.push_str(item.id.as_str());
        out.push(']');
        if !item.tags.is_empty() {
            out.push_str(" (");
            out.push_str(&item.tags.join(", "));
            out.push(')');
        }
        out.push(' ');
        out.push_str(&item.content);
        out.push('\n');
    }
    out
}
