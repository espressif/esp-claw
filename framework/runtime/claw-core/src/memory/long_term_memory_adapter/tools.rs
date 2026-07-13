//! Model-callable long-term memory tools over the dual-tier store.

mod args;

use claw_interface::ClawFs;
use claw_memory::{MemoryDraft, MemoryId, MemoryItem, MemoryPatch, StoreOutcome};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolGroup, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use self::args::{
    optional_limit, optional_string, optional_string_array, parse_object, required_string,
    string_array,
};
use super::MemoryStores;

pub(crate) fn memory_tools<F: ClawFs + 'static>(stores: MemoryStores<F>) -> ToolGroup {
    ToolGroup::new(
        "memory",
        true,
        [
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
        ],
    )
}

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
            .with_tags(string_array(&args, "tags")?)
            .with_keywords(string_array(&args, "keywords")?)
            .with_source("manual");

        let output = match self.stores.store(draft) {
            StoreOutcome::Created(item) => format!("Stored memory {}.", item.id),
            StoreOutcome::Duplicate(item) => {
                format!("Already remembered (as {}); nothing changed.", item.id)
            }
        };
        Ok(ToolOutput { output, ok: true })
    }
}

struct MemoryRecallTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryRecallTool<F> {
    tool_metadata!("memory_recall");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryRecallTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let labels = string_array(&args, "labels")?;
        let query = optional_string(&args, "query")?;
        let limit = optional_limit(&args)?;

        let items = self.stores.recall(&labels, query.as_deref(), limit);
        Ok(ToolOutput {
            output: render_items("Recalled memories", &items),
            ok: true,
        })
    }
}

struct MemoryListTool<F: ClawFs + 'static> {
    stores: MemoryStores<F>,
}

impl<F: ClawFs + 'static> ToolSpec for MemoryListTool<F> {
    tool_metadata!("memory_list");
}

impl<F: ClawFs + 'static> SyncToolHandler for MemoryListTool<F> {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_object(call)?;
        let limit = optional_limit(&args)?;
        let mut items = self.stores.list();
        items.truncate(limit);
        Ok(ToolOutput {
            output: render_items("All memories", &items),
            ok: true,
        })
    }
}

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
            content: optional_string(&args, "content")?,
            tags: optional_string_array(&args, "tags")?,
            keywords: optional_string_array(&args, "keywords")?,
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
